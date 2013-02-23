%% @author Northscale <info@northscale.com>
%% @copyright 2009 NorthScale, Inc.
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
%% Monitor and maintain the vbucket layout of each bucket.
%%
-module(ns_janitor).

-include("ns_common.hrl").

-include_lib("eunit/include/eunit.hrl").

-export([cleanup/2, current_states/2, graphviz/1]).

-define(WAIT_FOR_MEMCACHED_TRIES, 5).

-define(CLEAN_VBUCKET_MAP_RECOVER, 0).
-define(UNCLEAN_VBUCKET_MAP_RECOVER, 1).

-spec cleanup(string(), list()) -> ok | {error, any()}.
cleanup(Bucket, Options) ->
    {ok, Config} = ns_bucket:get_bucket(Bucket),
    case ns_bucket:bucket_type(Config) of
        membase -> do_cleanup(Bucket, Options, Config);
        _ -> ok
    end.

do_cleanup(Bucket, Options, Config) ->
    {Map, Servers} =
        case proplists:get_value(map, Config) of
            X when X == undefined; X == [] ->
                S = ns_cluster_membership:active_nodes(),
                Config1 = lists:keystore(servers, 1, Config, {servers, S}),
                NumVBuckets = proplists:get_value(num_vbuckets, Config1),
                NumReplicas = ns_bucket:num_replicas(Config1),
                ns_bucket:set_bucket_config(Bucket, Config1),
                wait_for_memcached(S, Bucket, up),
                NewMap = case read_existing_map(Bucket, S, NumVBuckets, NumReplicas) of
                             {ok, M} ->
                                 M;
                             {error, no_map} ->
                                 ns_rebalancer:generate_initial_map(Config1)
                         end,

                Config6 = lists:keystore(map, 1, Config1, {map, NewMap}),
                ns_bucket:set_bucket_config(Bucket, Config6),
                {NewMap, S};
            M ->
                {M, proplists:get_value(servers, Config)}
        end,
    case Servers of
        [] -> ok;
        _ ->
            case {wait_for_memcached(Servers, Bucket, connected, proplists:get_value(timeout, Options, 5)),
                  proplists:get_bool(best_effort, Options)} of
                {[_|_] = Down, false} ->
                    ?log_error("Bucket ~p not yet ready on ~p", [Bucket, Down]),
                    {error, wait_for_memcached_failed};
                {Down, _} ->
                    ReadyServers = ordsets:subtract(lists:sort(Servers),
                                                    lists:sort(Down)),
                    Map1 =
                        case sanify(Bucket, Map, ReadyServers, Down) of
                            Map -> Map;
                            MapNew ->
                                ns_bucket:set_map(Bucket, MapNew),
                                MapNew
                        end,
                    Replicas = lists:keysort(1, ns_bucket:map_to_replicas(Map1)),
                    ReplicaGroups = lists:ukeymerge(1, misc:keygroup(1, Replicas),
                                                    [{N, []} || N <- lists:sort(Servers)]),
                    NodesReplicas = lists:map(fun ({Src, R}) -> % R is the replicas for this node
                                                      {Src, [{V, Dst} || {_, Dst, V} <- R]}
                                              end, ReplicaGroups),
                    ns_vbm_sup:set_replicas(Bucket, NodesReplicas),
                    ok
            end
    end.


state_color(active) ->
    "color=green";
state_color(pending) ->
    "color=blue";
state_color(replica) ->
    "color=yellow";
state_color(dead) ->
    "color=red".


-spec node_vbuckets(non_neg_integer(), node(), [{node(), vbucket_id(),
                                                 vbucket_state()}], map()) ->
                           iolist().
node_vbuckets(I, Node, States, Map) ->
    GState = lists:keysort(1,
               [{VBucket, state_color(State)} || {N, VBucket, State} <- States,
                                                 N == Node]),
    GMap = [{VBucket, "color=gray"} || {VBucket, Chain} <- misc:enumerate(Map, 0),
                                         lists:member(Node, Chain)],
    [io_lib:format("n~Bv~B [style=filled label=\"~B\" group=g~B ~s];~n",
                   [I, V, V, V, Style]) ||
        {V, Style} <- lists:ukeymerge(1, GState, GMap)].

graphviz(Bucket) ->
    {ok, Config} = ns_bucket:get_bucket(Bucket),
    Map = proplists:get_value(map, Config, []),
    Servers = proplists:get_value(servers, Config, []),
    {ok, States, Zombies} = current_states(Servers, Bucket),
    Nodes = lists:sort(Servers),
    NodeColors = lists:map(fun (Node) ->
                                   case lists:member(Node, Zombies) of
                                       true -> {Node, "red"};
                                       false -> {Node, "black"}
                                   end
                           end, Nodes),
    SubGraphs = [io_lib:format("subgraph cluster_n~B {~ncolor=~s;~nlabel=\"~s\";~n~s}~n",
                              [I, Color, Node, node_vbuckets(I, Node, States, Map)]) ||
                    {I, {Node, Color}} <- misc:enumerate(NodeColors)],
    Replicants = lists:sort(ns_bucket:map_to_replicas(Map)),
    Replicators = lists:sort(ns_vbm_sup:replicators(Nodes, Bucket)),
    AllRep = lists:umerge(Replicants, Replicators),
    Edges = [io_lib:format("n~pv~B -> n~pv~B [color=~s];~n",
                            [misc:position(Src, Nodes), V,
                             misc:position(Dst, Nodes), V,
                             case {lists:member(R, Replicants), lists:member(R, Replicators)} of
                                 {true, true} -> "black";
                                 {true, false} -> "red";
                                 {false, true} -> "blue"
                             end]) ||
                R = {Src, Dst, V} <- AllRep],
    ["digraph G { rankdir=LR; ranksep=6;", SubGraphs, Edges, "}"].


-spec sanify(string(), map(), [atom()], [atom()]) -> map().
sanify(Bucket, Map, Servers, DownNodes) ->
    {ok, States, Zombies} = current_states(Servers, Bucket),
    [sanify_chain(Bucket, States, Chain, VBucket, Zombies ++ DownNodes)
     || {VBucket, Chain} <- misc:enumerate(Map, 0)].

sanify_chain(Bucket, State, Chain, VBucket, Zombies) ->
    NewChain = do_sanify_chain(Bucket, State, Chain, VBucket, Zombies),
    %% Fill in any missing replicas
    case length(NewChain) < length(Chain) of
        false ->
            NewChain;
        true ->
            NewChain ++ lists:duplicate(length(Chain) - length(NewChain),
                                        undefined)
    end.


do_sanify_chain(Bucket, States, Chain, VBucket, Zombies) ->
    NodeStates = [{N, S} || {N, V, S} <- States, V == VBucket],
    ChainStates = lists:map(fun (N) ->
                                    case lists:keyfind(N, 1, NodeStates) of
                                        false -> {N, case lists:member(N, Zombies) of
                                                         true -> zombie;
                                                         _ -> missing
                                                     end};
                                        X -> X
                                    end
                            end, Chain),
    ExtraStates = [X || X = {N, _} <- NodeStates,
                        not lists:member(N, Chain)],
    case ChainStates of
        [{undefined, _}|_] ->
            Chain;
        [{Master, State}|ReplicaStates] when State /= active andalso State /= zombie ->
            case [N || {N, active} <- ReplicaStates ++ ExtraStates] of
                [] ->
                    %% We'll let the next pass catch the replicas.
                    ?log_info("Setting vbucket ~p in ~p on ~p from ~p to active.",
                              [VBucket, Bucket, Master, State]),
                    ns_memcached:set_vbucket(Master, Bucket, VBucket, active),
                    Chain;
                [Node] ->
                    %% One active node, but it's not the master
                    case misc:position(Node, Chain) of
                        false ->
                            %% It's an extra node
                            ?log_warning(
                               "Master for vbucket ~p in ~p is not active, but ~p is, so making that the master.",
                              [VBucket, Bucket, Node]),
                            [Node];
                        Pos ->
                            [Node|lists:nthtail(Pos, Chain)]
                    end;
                Nodes ->
                    ?log_error(
                      "Extra active nodes ~p for vbucket ~p in ~p. This should never happen!",
                      [Nodes, Bucket, VBucket]),
                    Chain
            end;
        C = [{_, MasterState}|ReplicaStates] when MasterState =:= active orelse MasterState =:= zombie ->
            lists:foreach(
              fun ({_, {N, active}}) ->
                      ?log_error("Active replica ~p for vbucket ~p in ~p. "
                                 "This should never happen, but we have an "
                                 "active master, so I'm deleting it.",
                                 [N, Bucket]),
                      ns_memcached:set_vbucket(N, Bucket, VBucket, dead),
                      ns_vbm_sup:kill_children(N, Bucket, [VBucket]);
                  ({_, {_, replica}})-> % This is what we expect
                      ok;
                  ({_, {_, missing}}) ->
                      %% Either fewer nodes than copies or replicator
                      %% hasn't started yet
                      ok;
                  ({{_, zombie}, _}) -> ok;
                  ({_, {_, zombie}}) -> ok;
                  ({{undefined, _}, _}) -> ok;
                  ({{M, _}, _} = Pair) ->
                      ?log_info("Killing replicators for vbucket ~p on"
                                " master ~p because of ~p", [VBucket, M, Pair]),
                      ns_vbm_sup:kill_children(M, Bucket, [VBucket])
              end, misc:pairs(C)),
            HaveAllCopies = lists:all(
                              fun ({undefined, _}) -> false;
                                  ({_, replica}) -> true;
                                  (_) -> false
                              end, ReplicaStates),
            lists:foreach(
              fun ({N, State}) ->
                      case {HaveAllCopies, State} of
                          {true, dead} ->
                              ?log_info("Deleting dead vbucket ~p in ~p on ~p",
                                        [VBucket, Bucket, N]),
                              ns_memcached:delete_vbucket(N, Bucket, VBucket);
                          {true, _} ->
                              ?log_info("Deleting vbucket ~p in ~p on ~p",
                                        [VBucket, Bucket, N]),
                              ns_memcached:set_vbucket(
                                N, Bucket, VBucket, dead),
                              ns_memcached:delete_vbucket(N, Bucket, VBucket);
                          {false, dead} ->
                              ok;
                          {false, _} ->
                              ?log_info("Setting vbucket ~p in ~p on ~p from ~p"
                                        " to dead because we don't have all "
                                        "copies", [N, Bucket, VBucket, State]),
                              ns_memcached:set_vbucket(N, Bucket, VBucket, dead)
                      end
              end, ExtraStates),
            Chain;
        [{Master, State}|ReplicaStates] ->
            case [N||{N, RState} <- ReplicaStates ++ ExtraStates,
                     lists:member(RState, [active, pending, replica])] of
                [] ->
                    ?log_info("Setting vbucket ~p in ~p on master ~p to active",
                              [VBucket, Bucket, Master]),
                    ns_memcached:set_vbucket(Master, Bucket, VBucket,
                                                   active),
                    Chain;
                X ->
                    case lists:member(Master, Zombies) of
                        true -> ok;
                        false ->
                            ?log_error("Master ~p in state ~p for vbucket ~p in ~p but we have extra nodes ~p!",
                                       [Master, State, VBucket, Bucket, X])
                    end,
                    Chain
            end
    end.

%% [{Node, VBucket, State}...]
-spec current_states(list(atom()), string()) ->
                            {ok, list({atom(), integer(), atom()}), list(atom())}.
current_states(Nodes, Bucket) ->
    {Replies, DownNodes} = ns_memcached:list_vbuckets_multi(Nodes, Bucket),
    {GoodReplies, BadReplies} = lists:partition(fun ({_, {ok, _}}) -> true;
                                                    (_) -> false
                                                     end, Replies),
    ErrorNodes = [Node || {Node, _} <- BadReplies],
    States = [{Node, VBucket, State} || {Node, {ok, Reply}} <- GoodReplies,
                                        {VBucket, State} <- Reply],
    {ok, States, ErrorNodes ++ DownNodes}.

%%
%% Internal functions
%%

wait_for_memcached(Nodes, Bucket, Type) ->
    wait_for_memcached(Nodes, Bucket, Type, ?WAIT_FOR_MEMCACHED_TRIES).

wait_for_memcached(Nodes, Bucket, Type, Tries) when Tries > 0 ->
    ReadyNodes = ns_memcached:ready_nodes(Nodes, Bucket, Type, default),
    DownNodes = ordsets:subtract(ordsets:from_list(Nodes),
                                 ordsets:from_list(ReadyNodes)),
    case DownNodes of
        [] ->
            [];
        _ ->
            case Tries - 1 of
                0 ->
                    DownNodes;
                X ->
                    ?log_info("Waiting for ~p on ~p", [Bucket, DownNodes]),
                    timer:sleep(1000),
                    wait_for_memcached(Nodes, Bucket, Type, X)
            end
    end.

%% @doc check for existing vbucket states and generate a vbucket map from them
-spec read_existing_map(string(),
                        [atom()],
                        pos_integer(),
                        non_neg_integer()) -> {error, no_map} | {ok, list()}.
read_existing_map(Bucket, S, VBucketsCount, NumReplicas) ->

    VBuckets =
        [begin
             {ok, Buckets} = ns_memcached:list_vbuckets_prevstate(Node, Bucket),
             [{Index, State, Node} || {Index, State} <- Buckets]
         end || Node <- S],


    Fun = fun({Index, active, Node}, {Active, Replica}) ->
                  {[{Index, Node}|Active], Replica};
             ({Index, replica, Node}, {Active, Replica}) ->
                  {Active, dict:append(Index, Node, Replica)};
             (_, Acc) -> % Ignore Dead vbuckets
                  Acc
          end,

    {Active, Replica} =
        lists:foldl(Fun, {[], dict:new()}, lists:flatten(VBuckets)),

    {Map, HasDuplicates} = recover_map(Active, Replica, false, 0,
                                       VBucketsCount, []),
    MissingVBucketsCount = lists:foldl(fun ([undefined|_], Acc) -> Acc+1;
                                           (_, Acc) -> Acc
                                       end, 0, Map),
    case MissingVBucketsCount of
        VBucketsCount -> {error, no_map};
        _ ->
            CleanRecover = MissingVBucketsCount =:= 0
                andalso not HasDuplicates,
            case CleanRecover of
                true ->
                    ns_log:log(?MODULE, ?CLEAN_VBUCKET_MAP_RECOVER,
                               "Cleanly recovered vbucket map from data files for bucket: ~p~n",
                               [Bucket]);
                _ ->
                    ns_log:log(?MODULE, ?UNCLEAN_VBUCKET_MAP_RECOVER,
                               "Partially recovered vbucket map from data files for bucket: ~p."
                               ++ " Missing vbuckets count: ~p and HasDuplicates is: ~p~n",
                               [Bucket, MissingVBucketsCount, HasDuplicates])
            end,
            {ok, align_replicas(Map, NumReplicas)}
    end.

-spec recover_map([{non_neg_integer(), node()}],
                  dict(),
                  boolean(),
                  non_neg_integer(),
                  pos_integer(),
                  [[atom()]]) ->
                         {[[atom()]], boolean()}.
recover_map(_Active, _Replica, HasDuplicates, I,
            VBucketsCount, MapAcc) when I >= VBucketsCount ->
    {lists:reverse(MapAcc), HasDuplicates};
recover_map(Active, Replica, HasDuplicates, I, VBucketsCount, MapAcc) ->
    {NewDuplicates, Master} =
        case [N || {Ic, N} <- Active, Ic =:= I] of
            [MasterNode] ->
                {HasDuplicates, MasterNode};
            [] ->
                {HasDuplicates, undefined};
            [MasterNode,_|_] ->
                {true, MasterNode}
        end,
    NewMapAcc = [[Master | case dict:find(I, Replica) of
                               {ok, V} -> V;
                               error -> []
                           end]
                 | MapAcc],
    recover_map(Active, Replica, NewDuplicates, I+1, VBucketsCount, NewMapAcc).

-spec align_replicas([[atom()]], non_neg_integer()) ->
                            [[atom()]].
align_replicas(Map, NumReplicas) ->
    [begin
         [Master | Replicas] = Chain,
         [Master | align_chain_replicas(Replicas, NumReplicas)]
     end || Chain <- Map].

align_chain_replicas(_, 0) -> [];
align_chain_replicas([H|T] = _Chain, ReplicasLeft) ->
    [H | align_chain_replicas(T, ReplicasLeft-1)];
align_chain_replicas([] = _Chain, ReplicasLeft) ->
    lists:duplicate(ReplicasLeft, undefined).

-ifdef(EUNIT).

align_replicas_test() ->
    [[a, b, c],
     [d, e, undefined],
     [undefined, undefined, undefined]] =
        align_replicas([[a, b, c],
                        [d, e],
                        [undefined]], 2),

    [[a, b],
     [d, e],
     [undefined, undefined]] =
        align_replicas([[a, b, c],
                        [d, e],
                        [undefined]], 1),

    [[a],
     [d],
     [undefined]] =
        align_replicas([[a, b, c],
                        [d, e],
                        [undefined]], 0).

recover_map_test() ->
    Result1 = recover_map([{0, a},
                           {2, b},
                           {3, c},
                           {1, e}],
                          dict:from_list([{0, [c, d]},
                                          {1, [d, b]},
                                          {2, [e, d]},
                                          {3, [b, e]}]),
                          false, 0, 4, []),
    {[[a,c,d],
      [e,d,b],
      [b,e,d],
      [c,b,e]], false} = Result1,

    Result2 = recover_map([{0, a},
                           {2, b},
                           {1, e}],
                          dict:from_list([{0, [c, d]},
                                          {1, [d, b]},
                                          {2, [e, d]},
                                          {3, [b, e]}]),
                          false, 0, 4, []),

    {[[a,c,d],
      [e,d,b],
      [b,e,d],
      [undefined,b,e]], false} = Result2,

    Result3 = recover_map([{0, a},
                           {2, b},
                           {2, c},
                           {1, e}],
                          dict:from_list([{0, [c, d]},
                                          {1, [d, b]},
                                          {2, [e, d]},
                                          {3, [b, e]}]),
                          false, 0, 4, []),

    {[[a,c,d],
      [e,d,b],
      [b,e,d],
      [undefined,b,e]], true} = Result3.

-endif.
