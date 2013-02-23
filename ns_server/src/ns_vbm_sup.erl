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
-module(ns_vbm_sup).

-behaviour(supervisor).

-include("ns_common.hrl").

-record(child_id, {vbuckets::[non_neg_integer(), ...],
                   dest_node::atom()}).

-export([start_link/1,
         add_replica/4,
         kill_replica/4,
         kill_children/3,
         kill_dst_children/3,
         replicators/2,
         set_replicas/2,
         apply_changes/2,
         spawn_mover/4]).

-export([init/1]).

%%
%% API
%%
start_link(Bucket) ->
    supervisor:start_link({local, server(Bucket)}, ?MODULE, []).


%% @doc Add a vbucket to the filter for a given source and destination
%% node.
add_replica(Bucket, SrcNode, DstNode, VBucket) ->
    VBuckets =
        case get_replicator(Bucket, SrcNode, DstNode) of
            undefined ->
                [VBucket];
            #child_id{vbuckets=Vs} = Child ->
                kill_child(SrcNode, Bucket, Child),
                [VBucket|Vs]
        end,
    {ok, _} = start_child(SrcNode, Bucket, VBuckets, DstNode).


%% @doc Kill any replication for a given destination node and vbucket.
kill_replica(Bucket, SrcNode, DstNode, VBucket) ->
    case get_replicator(Bucket, SrcNode, DstNode) of
        undefined ->
            ok;
        #child_id{vbuckets=VBuckets} = Child ->
            %% Kill the child and start it again without this vbucket.
            kill_child(SrcNode, Bucket, Child),
            case VBuckets of
                [VBucket] ->
                    %% just kill when it was last vbucket
                    ?log_info("~p: killed last vbucket (~p) for destination ~p", [SrcNode, VBucket, DstNode]),
                    ok;
                _ ->
                    {ok, _} = start_child(SrcNode, Bucket, VBuckets -- [VBucket],
                                          DstNode)
            end
    end.

apply_changes(Bucket, ChangeTuples) ->
    ?log_info("Applying changes:~n~p~n", [ChangeTuples]),
    {ok, BucketConfig} = ns_bucket:get_bucket(Bucket),
    RelevantSrcNodes = proplists:get_value(servers, BucketConfig),
    true = (undefined =/= RelevantSrcNodes),
    ReplicatorsList =
        lists:flatmap(
          fun (Node) ->
                  Children = try children(Node, Bucket) catch _:_ -> [] end,
                  [{{Node, Dst}, lists:sort(VBuckets)}
                   || #child_id{vbuckets=VBuckets, dest_node=Dst} <- Children]
          end, RelevantSrcNodes),
    Replicators = dict:from_list(ReplicatorsList),
    NewReplicators =
        lists:foldl(fun ({Type, SrcNode, DstNode, VBucket}, CurrentReplicators) ->
                            Key = {SrcNode, DstNode},
                            VBuckets = case dict:find(Key, CurrentReplicators) of
                                           {ok, X} -> X;
                                           error -> []
                                       end,
                            NewVBuckets =
                                case Type of
                                    kill_replica ->
                                        ordsets:del_element(VBucket, VBuckets);
                                    add_replica ->
                                        ordsets:add_element(VBucket, VBuckets)
                                end,
                            dict:store(Key, NewVBuckets, CurrentReplicators)
                    end, Replicators, ChangeTuples),
    NewReplicasPre =
        dict:fold(fun ({SrcNode, DstNode}, VBuckets, Dict) ->
                          PrevValue = case dict:find(SrcNode, Dict) of
                                          error -> [];
                                          {ok, X} -> X
                                      end,
                          NewValue = [[{V, DstNode} || V <- VBuckets] | PrevValue],
                          dict:store(SrcNode, NewValue, Dict)
                  end, dict:new(), NewReplicators),
    NewReplicas = dict:map(fun (_K, V) -> lists:append(V) end, NewReplicasPre),
    ActualChangesCount =
        dict:fold(fun (K, V, Count) ->
                          case dict:find(K, Replicators) of
                              {ok, V} ->        % NOTE: V is bound
                                  Count;
                              _ -> Count+1
                          end
                  end, 0, NewReplicators)
        + dict:fold(fun (K, _V, Count) ->
                            case dict:find(K, NewReplicators) of
                                {ok, _} ->
                                    Count;
                                _ -> Count+1
                            end
                    end, 0, Replicators),
    NewReplicasList = dict:to_list(NewReplicas),
    %% ?log_info("NewReplicasList:~n~p~n", [NewReplicasList]),
    %% ?log_info("Replicators before:~n~p~n", [replicators([node() | nodes()], Bucket)]),
    set_replicas(Bucket, NewReplicasList),
    %% ?log_info("Replicators after:~n~p~n", [replicators([node() | nodes()], Bucket)]),
    ActualChangesCount.


replicators(Nodes, Bucket) ->
    lists:flatmap(
      fun (Node) ->
              try children(Node, Bucket) of
                  Children ->
                      [{Node, Dst, VBucket} ||
                          #child_id{vbuckets=VBuckets, dest_node=Dst}
                              <- Children,
                          VBucket <- VBuckets]
              catch
                  _:_ -> []
              end
      end, Nodes).

set_replicas(Bucket, NodesReplicas) ->
    %% Replace with the empty list if replication is disabled
    NR = case ns_config:search_node_prop(node(), ns_config:get(), replication,
                                    enabled, true) of
             true ->
                 NodesReplicas;
             false -> []
         end,
    LiveNodes = ns_node_disco:nodes_actual_proper(),
    %% Kill all replicators on nodes not in NR
    NodesWithoutReplicas = LiveNodes -- [N || {N, _} <- NR],
    lists:foreach(fun (Node) -> kill_all_children(Node, Bucket) end,
                  NodesWithoutReplicas),
    lists:foreach(
      fun ({Src, R}) ->
              case lists:member(Src, LiveNodes) of
                  true ->
                      try set_replicas(Src, Bucket, R)
                      catch
                          E:R ->
                              ?log_error("Unable to start replicators on ~p for bucket ~p: ~p",
                                         [Src, Bucket, {E, R}])
                      end;
                  false ->
                      ok
              end
      end, NR).


spawn_mover(Bucket, VBucket, SrcNode, DstNode) ->
    Args = args(SrcNode, Bucket, [VBucket], DstNode, true),
    case apply(ebucketmigrator_srv, start_link, Args) of
        {ok, Pid} = RV ->
            ?log_info("Spawned mover ~p ~p ~p -> ~p: ~p", [Bucket, VBucket, SrcNode, DstNode, Pid]),
            RV;
        X -> X
    end.

-spec kill_all_children(node(), bucket_name()) ->
                               ok.
kill_all_children(Node, Bucket) ->
    try children(Node, Bucket) of
        Children ->
            lists:foreach(fun (Child) -> kill_child(Node, Bucket, Child) end,
                          Children)
    catch exit:{noproc, _} ->
            %% If the supervisor isn't running, obviously there's no
            %% replication.
            ok
    end.



-spec kill_child(node(), nonempty_string(), #child_id{}) ->
                        ok.
kill_child(Node, Bucket, Child) ->
    ok = supervisor:terminate_child({server(Bucket), Node}, Child),
    ok = supervisor:delete_child({server(Bucket), Node}, Child).


-spec kill_children(node(), nonempty_string(), [non_neg_integer()]) ->
                           [#child_id{}].
kill_children(Node, Bucket, VBuckets) ->
    %% Kill any existing children for these VBuckets
    Children = [Id || Id = #child_id{vbuckets=Vs} <-
                          children(Node, Bucket),
                      Vs -- VBuckets /= Vs],
    lists:foreach(fun (Child) ->
                          kill_child(Node, Bucket, Child)
                  end, Children),
    Children.

-spec kill_dst_children(node(), nonempty_string(), node()) ->
                               ok.
kill_dst_children(Node, Bucket, Dst) ->
    Children = [Id || Id = #child_id{dest_node=D} <- children(Node, Bucket),
                      D == Dst],
    lists:foreach(fun (Child) ->
                          kill_child(Node, Bucket, Child)
                  end, Children).

%% supervisor callbacks
init([]) ->
    {ok, {{one_for_one,
           misc:get_env_default(max_r, 3),
           misc:get_env_default(max_t, 10)},
          []}}.

%%
%% Internal functions
%%

-spec args(atom(), nonempty_string(), [non_neg_integer(),...], atom(), boolean()) ->
                  [any(), ...].
args(Node, Bucket, VBuckets, DstNode, TakeOver) ->
    {User, Pass} = ns_bucket:credentials(Bucket),
    Suffix = case TakeOver of
                 true ->
                     [VBucket] = VBuckets,
                     integer_to_list(VBucket);
                 false ->
                     %% We want to reuse names for replication.
                     atom_to_list(DstNode)
             end,
    [ns_memcached:host_port(Node), ns_memcached:host_port(DstNode),
     [{username, User},
      {password, Pass},
      {vbuckets, VBuckets},
      {takeover, TakeOver},
      {suffix, Suffix}]].

-spec children(node(), nonempty_string()) -> [#child_id{}].
children(Node, Bucket) ->
    [Id || {Id, _, _, _} <- supervisor:which_children({server(Bucket), Node})].


%% @private
%% @doc Get the replicator for a given source and destination node.
-spec get_replicator(nonempty_string(), node(), node()) ->
                            #child_id{} | undefined.
get_replicator(Bucket, SrcNode, DstNode) ->
    case [Id || {#child_id{dest_node=N} = Id, _, _, _}
                    <- supervisor:which_children({server(Bucket), SrcNode}),
                N == DstNode] of
        [Child] ->
            Child;
        [] ->
            undefined
    end.



-spec server(nonempty_string()) ->
                    atom().
server(Bucket) ->
    list_to_atom(?MODULE_STRING "-" ++ Bucket).


%% @doc Set up replication from the given source node to a list of
%% {VBucket, DstNode} pairs.
set_replicas(SrcNode, Bucket, Replicas) ->
    %% A dictionary mapping destination node to a sorted list of
    %% vbuckets to replicate there from SrcNode.
    DesiredReplicaDict =
        dict:map(
          fun (_, V) -> lists:usort(V) end,
          lists:foldl(
            fun ({VBucket, DstNode}, D) ->
                    dict:append(DstNode, VBucket, D)
            end, dict:new(), Replicas)),
    %% Remove any destination nodes from the dictionary that the
    %% replication is already correct for.
    NeededReplicas =
        dict:to_list(
          lists:foldl(
            fun (#child_id{dest_node=DstNode, vbuckets=VBuckets} = Id, D) ->
                    case dict:find(DstNode, DesiredReplicaDict) of
                        {ok, VBuckets} ->
                            %% Already running
                            dict:erase(DstNode, D);
                        _ ->
                            %% Either wrong vbuckets or not wanted at all
                            kill_child(SrcNode, Bucket, Id),
                            D
                    end
            end, DesiredReplicaDict, children(SrcNode, Bucket))),
    lists:foreach(
      fun ({DstNode, VBuckets}) ->
              start_child(SrcNode, Bucket, VBuckets, DstNode)
      end, NeededReplicas).


-spec start_child(atom(), nonempty_string(), [non_neg_integer(),...], atom()) ->
                         {ok, pid()}.
start_child(Node, Bucket, VBuckets, DstNode) ->
    Args = args(Node, Bucket, VBuckets, DstNode, false),
    ?log_info("Starting replicator with args =~n~p",
              [Args]),
    ChildSpec = {#child_id{vbuckets=VBuckets, dest_node=DstNode},
                 {ebucketmigrator_srv, start_link, Args},
                 permanent, 60000, worker, [ebucketmigrator_srv]},
    supervisor:start_child({server(Bucket), Node}, ChildSpec).
