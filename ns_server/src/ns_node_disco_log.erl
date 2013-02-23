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
-module(ns_node_disco_log).

-behaviour(gen_event).

-export([start_link/0]).

%% gen_event callbacks
-export([init/1, handle_event/2, handle_call/2,
         handle_info/2, terminate/2, code_change/3]).

-record(state, {}).

start_link() ->
    {ok, spawn_link(fun() ->
                            gen_event:add_sup_handler(ns_node_disco_events,
                                                      ?MODULE, ignored),
                            receive
                                _ -> ok
                            end
                    end)}.

init(ignored) ->
    {ok, #state{}, hibernate}.

terminate(_Reason, _State)     -> ok.
code_change(_OldVsn, State, _) -> {ok, State}.

handle_event({ns_node_disco_events, _NodesBefore, NodesAfter}, State) ->
    error_logger:info_msg("ns_node_disco_log: nodes changed: ~p~n",
                          [NodesAfter]),
    {ok, State, hibernate};

handle_event(_, State) ->
    {ok, State, hibernate}.

handle_call(Request, State) ->
    error_logger:info_msg("handle_call(~p, ~p, ~p)~n", [?MODULE, Request, State]),
    {ok, ok, State, hibernate}.

handle_info(_Info, State) ->
    {ok, State, hibernate}.
