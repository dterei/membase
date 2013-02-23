%% @author Couchbase <info@couchbase.com>
%% @copyright 2011 Couchbase, Inc.
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
%% @doc This gen_event handler holds failover safeness level for some
%% particular bucket. We estimate failover safeness as approximation
%% of time required to replicate backlog of un-replicated yet items.
%%
%% The approach is to subscribe to stats and keep moving averages of
%% replication tap streams backlog and replication tap streams drain
%% rate. After each new sample we update averages and compute
%% estimated time to drain all replication backlog. If that estimate is
%% higher then 2000 ms we switch to 'yellow' level. Switching back to
%% green has much lower bound to avoid too frequent changing of levels
%% (i.e. hysteresis).
%%
%% We also keep track of last time we've seen any stats sample. And we
%% use it when asked about current safeness level. If our information
%% is too stale (5 or more seconds old), then we respond with 'stale'
%% level.

-module(failover_safeness_level).

-behaviour(gen_event).

-include("ns_stats.hrl").

-define(TO_YELLOW_SECONDS, 2.0).
-define(FROM_YELLOW_SECONDS, 1.0).
-define(STALENESS_THRESHOLD, 5000).
-define(TOO_SMALL_BACKLOG, 0.5).

-export([start_link/1, safeness_level/1]).

%% gen_event callbacks
-export([init/1, handle_event/2, handle_call/2,
         handle_info/2, terminate/2, code_change/3]).

-record(state,
        {bucket_name :: string(),
         backlog_size :: undefined | number(),
         drain_rate :: undefined | number(),
         safeness_level = unknown :: yellow | green | unknown,
         last_ts :: undefined | non_neg_integer(),
         last_update_local_clock :: non_neg_integer()
        }).

start_link(BucketName) ->
    misc:start_event_link(
      fun() ->
              ok = gen_event:add_sup_handler(ns_stats_event,
                                             {?MODULE, {?MODULE, BucketName}},
                                             [BucketName])
      end).

-spec safeness_level(string()) ->
                            {ok, stale | unknown | green | yellow} | {error, term()}.
safeness_level(BucketName) ->
    gen_event:call(ns_stats_event, {?MODULE, {?MODULE, BucketName}}, get_level, 2000).

init([BucketName]) ->
    {ok, #state{bucket_name = BucketName,
                last_update_local_clock = misc:time_to_epoch_ms_int(now())}}.

terminate(_Reason, _State)     -> ok.
code_change(_OldVsn, State, _) -> {ok, State}.

handle_event({stats, StatsBucket, #stat_entry{timestamp = TS,
                                              values = Values}},
             #state{bucket_name = StatsBucket} = State) ->
    case {orddict:find(ep_tap_replica_total_backlog_size, Values),
          orddict:find(ep_tap_replica_queue_drain, Values)} of
        {{ok, BacklogSize},
         {ok, DrainRate}} ->
            handle_stats_sample(BacklogSize, DrainRate, TS, State);
        _ ->
            {ok, mark_update(State#state{last_ts = TS})}
    end;

handle_event(_, State) ->
    {ok, State}.

%% window in milliseconds of averaging of stats used in computation of
%% failover safeness
-define(AVERAGING_WINDOW, 7000).

%% this implements simple and cheap averaging method known as
%% exponential averaging. See here:
%% http://en.wikipedia.org/wiki/Moving_average#Application_to_measuring_computer_performance
average_value(undefined, New, _LastTS, _TS) -> New;
average_value(Old, New, LastTS, TS) ->
    case LastTS < TS of
        false -> New;
        _ ->
            Beta = math:exp((LastTS - TS) / ?AVERAGING_WINDOW),
            Alpha = 1 - Beta,
            New * Alpha + Beta * Old
    end.

new_safeness_level(Level, BacklogSize, DrainRate) when Level =/= green ->
    try BacklogSize < ?TOO_SMALL_BACKLOG orelse BacklogSize / DrainRate =< ?FROM_YELLOW_SECONDS of
        true -> green;
        _ -> yellow
    catch error:badarith ->
            %% division by zero or overflow/underflow (erlang doesn't have infinities)
            case BacklogSize > DrainRate of
                true ->
                    %% overflow
                    yellow;
                _ ->
                    %% underflow or 0/0
                    green
            end
    end;
new_safeness_level(green, BacklogSize, DrainRate) ->
    try BacklogSize >= ?TOO_SMALL_BACKLOG andalso BacklogSize / DrainRate > ?TO_YELLOW_SECONDS of
        true -> yellow;
        _ -> green
    catch error:badarith ->
            %% division by zero or overflow/underflow (erlang doesn't have infinities)
            case BacklogSize > DrainRate of
                true ->
                    %% overflow
                    yellow;
                _ ->
                    %% underflow or 0/0
                    green
            end
    end.


mark_update(State) ->
    State#state{last_update_local_clock = misc:time_to_epoch_ms_int(now())}.

handle_stats_sample(BacklogSize, DrainRate, TS,
                    #state{backlog_size = OldBacklogSize,
                           drain_rate = OldDrainRate,
                           last_ts = LastTS,
                           safeness_level = SafenessLevel} = State) ->
    NewBacklogSize = average_value(OldBacklogSize, BacklogSize,
                                   LastTS, TS),
    NewDrainRate = average_value(OldDrainRate, DrainRate,
                                 LastTS, TS),
    NewSafenessLevel = new_safeness_level(SafenessLevel, NewBacklogSize, NewDrainRate),
    {ok, mark_update(State#state{
                       backlog_size = NewBacklogSize,
                       drain_rate = NewDrainRate,
                       last_ts = TS,
                       safeness_level = NewSafenessLevel})}.

handle_call(get_level, #state{last_update_local_clock = UpdateTS,
                              safeness_level = Level} = State) ->
    Now = misc:time_to_epoch_ms_int(now()),
    RV = case Now - UpdateTS > ?STALENESS_THRESHOLD of
             true ->
                 stale;
             _ ->
                 Level
         end,
    {ok, {ok, RV}, State};
handle_call(get_state, State) ->
    {ok, State, State}.


handle_info(_Info, State) ->
    {ok, State}.
