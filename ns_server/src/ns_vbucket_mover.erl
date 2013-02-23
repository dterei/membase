%% @author Northscale <info@northscale.com>
%% @copyright 2010 NorthScale, Inc.
%%
%% Licensed under the Apache License, Version 2.0 (the "License");
%% you may not use this file except in compliance with the License.
%% You may obtain a copy of the License at
%%
%%      http://www.apache.org/licenses/LICENSE-2.0
%%
%% Unless required by applicable law or agreed to in writing, software
%% distributed under the License is distributed on an "AS IS" BASIS,
%% WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
%% See the License for the specific language governing permissions and
%% limitations under the License.
%%
-module(ns_vbucket_mover).

-behavior(gen_server).

-include("ns_common.hrl").

-include_lib("eunit/include/eunit.hrl").

-define(MAX_MOVES_PER_NODE, 1).

%% API
-export([start_link/4, stop/1]).

%% gen_server callbacks
-export([code_change/3, init/1, handle_call/3, handle_cast/2, handle_info/2,
         terminate/2]).

-type progress_callback() :: fun((dict()) -> any()).

-record(state, {bucket::nonempty_string(),
                previous_changes,
                initial_counts::dict(),
                max_per_node::pos_integer(),
                map::array(),
                moves::dict(), movers::dict(),
                progress_callback::progress_callback()}).

%%
%% API
%%

%% @doc Start the mover.
-spec start_link(string(), map(), map(), progress_callback()) ->
                        {ok, pid()} | {error, any()}.
start_link(Bucket, OldMap, NewMap, ProgressCallback) ->
    gen_server:start_link(?MODULE, {Bucket, OldMap, NewMap, ProgressCallback},
                          []).


%% @doc Stop the in-progress moves.
-spec stop(pid()) -> ok.
stop(Pid) ->
    gen_server:call(Pid, stop).





%%
%% gen_server callbacks
%%

code_change(_OldVsn, _Extra, State) ->
    {ok, State}.


init({Bucket, OldMap, NewMap, ProgressCallback}) ->
    erlang:put(i_am_master_mover, true),
    erlang:put(replicas_changes, []),
    erlang:put(bucket_name, Bucket),
    erlang:put(total_changes, 0),
    erlang:put(actual_changes, 0),

    ?log_info("Starting movers with new map =~n~p", [NewMap]),
    %% Dictionary mapping old node to vbucket and new node
    MoveDict = lists:foldl(fun ({V, [M1|_] = C1, C2}, D) ->
                                   dict:append(M1, {V, C1, C2}, D)
                           end, dict:new(),
                           lists:zip3(lists:seq(0, length(OldMap) - 1), OldMap,
                                      NewMap)),
    Movers = dict:map(fun (_, _) -> 0 end, MoveDict),
    self() ! spawn_initial,
    process_flag(trap_exit, true),
    erlang:start_timer(3000, self(), maybe_sync_changes),
    {ok, #state{bucket=Bucket,
                previous_changes = [],
                initial_counts=count_moves(MoveDict),
                max_per_node=?MAX_MOVES_PER_NODE,
                map = map_to_array(OldMap),
                moves=MoveDict, movers=Movers,
                progress_callback=ProgressCallback}}.


handle_call(stop, _From, State) ->
    %% All the linked processes should exit when we do.
    {stop, normal, ok, State}.


handle_cast(unhandled, unhandled) ->
    unhandled.


%% We intentionally don't handle other exits so we'll die if one of
%% the movers fails.
handle_info({_, _, maybe_sync_changes}, #state{previous_changes = PrevChanges} = State) ->
    Changes = erlang:get('replicas_changes'),
    case Changes =:= [] orelse Changes =/= PrevChanges of
        true -> {noreply, State#state{previous_changes = Changes}};
        _ ->
            sync_replicas(),
            {noreply, State#state{previous_changes = []}}
    end;
handle_info(spawn_initial, State) ->
    spawn_workers(State);
handle_info({move_done, {Node, VBucket, OldChain, NewChain}},
            #state{movers=Movers, map=Map} = State) ->
    %% Update replication
    update_replication_post_move(VBucket, OldChain, NewChain),
    %% Free up a mover for this node
    Movers1 = dict:update(Node, fun (N) -> N - 1 end, Movers),
    %% Pull the new chain from the target map
    %% Update the current map
    Map1 = array:set(VBucket, NewChain, Map),
    spawn_workers(State#state{movers=Movers1, map=Map1});
handle_info({'EXIT', _, normal}, State) ->
    {noreply, State};
handle_info({'EXIT', Pid, Reason}, State) ->
    ?log_error("~p exited with ~p", [Pid, Reason]),
    {stop, Reason, State};
handle_info(Info, State) ->
    ?log_info("Unhandled message ~p", [Info]),
    {noreply, State}.


terminate(_Reason, #state{bucket=Bucket, map=MapArray}) ->
    sync_replicas(),
    TotalChanges = erlang:get(total_changes),
    ActualChanges = erlang:get(actual_changes),
    ?log_info("Savings: ~p (from ~p)~n", [TotalChanges - ActualChanges, TotalChanges]),
    %% Save the current state of the map
    Map = array_to_map(MapArray),
    ?log_info("Setting final map to ~p", [Map]),
    ns_bucket:set_map(Bucket, Map),
    ok.


%%
%% Internal functions
%%

%% @private
%% @doc Convert a map array back to a map list.
-spec array_to_map(array()) ->
                          map().
array_to_map(Array) ->
    array:to_list(Array).


%% @private
%% @doc Count of remaining moves per node.
-spec count_moves(dict()) -> dict().
count_moves(Moves) ->
    %% Number of moves FROM a given node.
    FromCount = dict:map(fun (_, M) -> length(M) end, Moves),
    %% Add moves TO each node.
    dict:fold(fun (_, M, D) ->
                      lists:foldl(
                        fun ({_, _, [N|_]}, E) ->
                                dict:update_counter(N, 1, E)
                        end, D, M)
              end, FromCount, Moves).


%% @private
%% @doc Convert a map, which is normally a list, into an array so that
%% we can randomly access the replication chains.
-spec map_to_array(map()) ->
                          array().
map_to_array(Map) ->
    array:fix(array:from_list(Map)).


%% @private
%% @doc {Src, Dst} pairs from a chain with unmapped nodes filtered out.
pairs(Chain) ->
    [Pair || {Src, Dst} = Pair <- misc:pairs(Chain), Src /= undefined,
             Dst /= undefined].


%% @private
%% @doc Report progress using the supplied progress callback.
-spec report_progress(#state{}) -> any().
report_progress(#state{initial_counts=Counts, moves=Moves,
                       progress_callback=Callback}) ->
    Remaining = count_moves(Moves),
    Progress = dict:map(fun (Node, R) ->
                                Total = dict:fetch(Node, Counts),
                                1.0 - R / Total
                        end, Remaining),
    Callback(Progress).


%% @private
%% @doc Spawn workers up to the per-node maximum.
-spec spawn_workers(#state{}) -> {noreply, #state{}} | {stop, normal, #state{}}.
spawn_workers(#state{bucket=Bucket, moves=Moves, movers=Movers,
                     max_per_node=MaxPerNode} = State) ->
    report_progress(State),
    Parent = self(),
    {Movers1, Remaining} =
        dict:fold(
          fun (Node, RemainingMoves, {M, R}) ->
                  NumWorkers = dict:fetch(Node, Movers),
                  if NumWorkers < MaxPerNode ->
                          NewMovers = lists:sublist(RemainingMoves,
                                                    MaxPerNode - NumWorkers),
                          lists:foreach(
                            fun ({V, OldChain, [NewNode|_] = NewChain}) ->
                                    update_replication_pre_move(
                                      V, OldChain, NewChain),
                                    spawn_link(
                                      case Node of
                                          undefined -> node();
                                          _ -> Node
                                      end,
                                      fun () ->
                                              process_flag(trap_exit, true),
                                              %% We do a no-op here
                                              %% rather than filtering
                                              %% these out so that the
                                              %% replication update
                                              %% will still work
                                              %% properly.
                                              if
                                                  Node =:= undefined ->
                                                      %% this handles
                                                      %% case of
                                                      %% missing
                                                      %% vbucket (like
                                                      %% after failing
                                                      %% over more
                                                      %% nodes then
                                                      %% replica
                                                      %% count)
                                                      ok;
                                                  Node /= NewNode ->
                                                      run_mover(Bucket, V, Node,
                                                                NewNode, 2);
                                                 true ->
                                                      ok
                                              end,
                                              Parent ! {move_done,
                                                        {Node, V, OldChain,
                                                         NewChain}}
                                      end)
                            end, NewMovers),
                          M1 = dict:store(Node, length(NewMovers) + NumWorkers,
                                          M),
                          R1 = dict:store(Node, lists:nthtail(length(NewMovers),
                                                              RemainingMoves), R),
                          {M1, R1};
                     true ->
                          {M, R}
                  end
          end, {Movers, Moves}, Moves),
    State1 = State#state{movers=Movers1, moves=Remaining},
    Values = dict:fold(fun (_, V, L) -> [V|L] end, [], Movers1),
    case Values /= [] andalso lists:any(fun (V) -> V /= 0 end, Values) of
        true ->
            {noreply, State1};
        false ->
            {stop, normal, State1}
    end.


run_mover(Bucket, V, N1, N2, Tries) ->
    case {ns_memcached:get_vbucket(N1, Bucket, V),
          ns_memcached:get_vbucket(N2, Bucket, V)} of
        {{ok, active}, {memcached_error, not_my_vbucket, _}} ->
            %% Standard starting state
            ok = ns_memcached:set_vbucket(N2, Bucket, V, replica),
            {ok, _Pid} = ns_vbm_sup:spawn_mover(Bucket, V, N1, N2),
            wait_for_mover(Bucket, V, N1, N2, Tries);
        {{ok, dead}, {ok, active}} ->
            %% Standard ending state
            ok;
        {{memcached_error, not_my_vbucket, _}, {ok, active}} ->
            %% This generally shouldn't happen, but it's an OK final state.
            ?log_warning("Weird: vbucket ~p missing from source node ~p but "
                         "active on destination node ~p.", [V, N1, N2]),
            ok;
        {{ok, active}, {ok, S}} when S /= active ->
            %% This better have been a replica, a failed previous
            %% attempt to migrate, or loaded from a valid copy or this
            %% will result in inconsistent data!
            if S /= replica ->
                    ok = ns_memcached:set_vbucket(N2, Bucket, V, replica);
               true ->
                    ok
            end,
            {ok, _Pid} = ns_vbm_sup:spawn_mover(Bucket, V, N1, N2),
            wait_for_mover(Bucket, V, N1, N2, Tries);
        {{ok, dead}, {ok, pending}} ->
            %% This is a strange state to end up in - the source
            %% shouldn't close until the destination has acknowledged
            %% the last message, at which point the state should be
            %% active.
            ?log_warning("Weird: vbucket ~p in pending state on node ~p.",
                         [V, N2]),
            ok = ns_memcached:set_vbucket(N1, Bucket, V, active),
            ok = ns_memcached:set_vbucket(N2, Bucket, V, replica),
            {ok, _Pid} = ns_vbm_sup:spawn_mover(Bucket, V, N1, N2),
            wait_for_mover(Bucket, V, N1, N2, Tries)
    end.


%% @private
%% @doc Perform pre-move replication fixup.
update_replication_pre_move(VBucket, OldChain, NewChain) ->
    [NewMaster|_] = NewChain,
    OldPairs = pairs(OldChain),
    NewPairs = pairs(NewChain),
    %% Stop replication to the new master
    case lists:keyfind(NewMaster, 2, OldPairs) of
        {SrcNode, _} ->
            kill_replica(SrcNode, NewMaster, VBucket),
            sync_replicas();
        false ->
            ok
    end,
    %% Start replication to any new replicas that aren't already being
    %% replicated to
    lists:foreach(
      fun ({SrcNode, DstNode}) ->
              case lists:member(DstNode, OldChain) of
                  false ->
                      %% Not the old master or already being replicated to
                      add_replica(SrcNode, DstNode, VBucket);
                  true ->
                      %% Already being replicated to; swing it over after
                      ok
              end
      end, NewPairs -- OldPairs).


%% @private
%% @doc Perform post-move replication fixup.
update_replication_post_move(VBucket, OldChain, NewChain) ->
    OldPairs = pairs(OldChain),
    NewPairs = pairs(NewChain),
    %% Stop replication for any old pair that isn't needed any more.
    lists:foreach(
      fun ({SrcNode, DstNode}) when SrcNode =/= undefined ->
              kill_replica(SrcNode, DstNode, VBucket)
      end, OldPairs -- NewPairs),
    %% Start replication for any new pair that wouldn't have already
    %% been started.
    lists:foreach(
      fun ({SrcNode, DstNode}) ->
              case lists:member(DstNode, OldChain) of
                  false ->
                      %% Would have already been started
                      ok;
                  true ->
                      %% Old one was stopped by the previous loop
                      add_replica(SrcNode, DstNode, VBucket)
              end
      end, NewPairs -- OldPairs),
    %% TODO: wait for backfill to complete and remove obsolete
    %% copies before continuing. Otherwise rebalance could use a lot
    %% of space.
    ok.

assert_master_mover() ->
    true = erlang:get('i_am_master_mover'),
    BucketName = erlang:get('bucket_name'),
    true = (BucketName =/= undefined),
    BucketName.

batch_replicas_change(Tuple) ->
    assert_master_mover(),
    Old = erlang:get('replicas_changes'),
    true = (undefined =/= Old),
    New = [Tuple | Old],
    erlang:put(replicas_changes, New).

kill_replica(SrcNode, DstNode, VBucket) ->
    assert_master_mover(),
    batch_replicas_change({kill_replica, SrcNode, DstNode, VBucket}).

add_replica(SrcNode, DstNode, VBucket) ->
    assert_master_mover(),
    batch_replicas_change({add_replica, SrcNode, DstNode, VBucket}).

inc_counter(Name, By) ->
    Old = erlang:get(Name),
    true = (undefined =/= Old),
    erlang:put(Name, Old + By).

sync_replicas() ->
    BucketName = assert_master_mover(),
    case erlang:put(replicas_changes, []) of
        undefined -> ok;
        [] -> ok;
        Changes ->
            ActualCount = ns_vbm_sup:apply_changes(BucketName, lists:reverse(Changes)),
            inc_counter(total_changes, length(Changes)),
            inc_counter(actual_changes, ActualCount)
    end.


wait_for_mover(Bucket, V, N1, N2, Tries) ->
    receive
        {'EXIT', _Pid, normal} ->
            case {ns_memcached:get_vbucket(N1, Bucket, V),
                  ns_memcached:get_vbucket(N2, Bucket, V)} of
                {{ok, dead}, {ok, active}} ->
                    ok;
                E ->
                    exit({wrong_state_after_transfer, E, V})
            end;
        {'EXIT', _Pid, stopped} ->
            exit(stopped);
        {'EXIT', _Pid, Reason} ->
            case Tries of
                0 ->
                    exit({mover_failed, Reason});
                _ ->
                    ?log_warning("Got unexpected exit reason from mover:~n~p",
                                 [Reason]),
                    run_mover(Bucket, V, N1, N2, Tries-1)
            end;
        Msg ->
            ?log_warning("Mover parent got unexpected message:~n"
                         "~p", [Msg]),
            wait_for_mover(Bucket, V, N1, N2, Tries)
    end.
