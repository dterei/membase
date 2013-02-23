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
-module(ns_doctor).

-define(STALE_TIME, 5000000). % 5 seconds in microseconds
-define(LOG_INTERVAL, 60000). % How often to dump current status to the logs

-include("ns_common.hrl").

-behaviour(gen_server).
-export([start_link/0]).
-export([init/1, handle_call/3, handle_cast/2, handle_info/2, terminate/2,
         code_change/3]).
%% API
-export([get_nodes/0]).

-record(state, {nodes}).

%% gen_server handlers

start_link() ->
    gen_server:start_link({local, ?MODULE}, ?MODULE, [], []).

init([]) ->
    self() ! acquire_initial_status,
    case misc:get_env_default(dont_log_stats, false) of
        false ->
            timer:send_interval(?LOG_INTERVAL, log);
        _ -> ok
    end,
    {ok, #state{nodes=dict:new()}}.

handle_call(get_nodes, _From, #state{nodes=Nodes} = State) ->
    Now = erlang:now(),
    LiveNodes = [node()|nodes()],
    Nodes1 = dict:map(
               fun (Node, Status) ->
                       LastHeard = proplists:get_value(last_heard, Status),
                       Stale = case timer:now_diff(Now, LastHeard) of
                                   T when T > ?STALE_TIME ->
                                       [ stale | Status];
                                   _ -> Status
                               end,
                       case lists:member(Node, LiveNodes) of
                           true ->
                               Stale;
                           false ->
                               [ down | Stale ]
                       end
               end, Nodes),
    {reply, Nodes1, State}.


handle_cast({heartbeat, Name, Status}, State) ->
    Nodes = update_status(Name, Status, State#state.nodes),
    {noreply, State#state{nodes=Nodes}};

handle_cast(Msg, State) ->
    ?log_warning("Unexpected cast: ~p", [Msg]),
    {noreply, State}.


handle_info(acquire_initial_status, #state{nodes=NodeDict} = State) ->
    Replies = ns_heart:status_all(),
    %% Get an initial status so we don't start up thinking everything's down
    Nodes = lists:foldl(fun ({Node, Status}, Dict) ->
                                update_status(Node, Status, Dict)
                        end, NodeDict, Replies),
    ?log_info("Got initial status ~p~n", [lists:sort(dict:to_list(Nodes))]),
    {noreply, State#state{nodes=Nodes}};

handle_info(log, #state{nodes=NodeDict} = State) ->
    ?log_info("Current node statuses:~n~p",
              [lists:sort(dict:to_list(NodeDict))]),
    {noreply, State};

handle_info(Info, State) ->
    ?log_warning("Unexpected message ~p in state",
                 [Info]),
    {noreply, State}.

terminate(_Reason, _State) -> ok.

code_change(_OldVsn, State, _Extra) -> {ok, State}.

%% API

get_nodes() ->
    try gen_server:call(?MODULE, get_nodes) of
        Nodes -> Nodes
    catch
        E:R ->
            ?log_error("Error attempting to get nodes: ~p", [{E, R}]),
            dict:new()
    end.


%% Internal functions

is_significant_buckets_change(OldStatus, NewStatus) ->
    [OldActiveBuckets, OldReadyBuckets,
     NewActiveBuckets, NewReadyBuckets] =
        [lists:sort(proplists:get_value(Field, Status, []))
         || Status <- [OldStatus, NewStatus],
            Field <- [active_buckets, ready_buckets]],
    OldActiveBuckets =/= NewActiveBuckets
        orelse OldReadyBuckets =/= NewReadyBuckets.

update_status(Name, Status0, Dict) ->
    Status = [{last_heard, erlang:now()} | Status0],
    PrevStatus = case dict:find(Name, Dict) of
                     {ok, V} -> V;
                     error -> []
                 end,
    case is_significant_buckets_change(PrevStatus, Status) of
        true ->
            gen_event:notify(buckets_events, {significant_buckets_change, Name});
        _ ->
            ok
    end,
    dict:store(Name, Status, Dict).
