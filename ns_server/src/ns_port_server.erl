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
-module(ns_port_server).

-define(ABNORMAL, 0).
-define(LF_DEATH_TIMEOUT, 60000).

-behavior(gen_server).

-include("ns_common.hrl").

%% API
-export([start_link/4]).

%% gen_server callbacks
-export([init/1,
         handle_call/3,
         handle_cast/2,
         handle_info/2,
         code_change/3,
         terminate/2]).

-define(NUM_MESSAGES, 3). % Number of the most recent messages to log on crash
-define(MAX_MESSAGES, 10). % Max messages per interval
-define(INTERVAL, 1000). % Interval over which to throttle

-define(UNEXPECTED, 1).

-include_lib("eunit/include/eunit.hrl").

%% Server state
-record(state, {port :: port(),
                name :: term(),
                messages,
                log_tref :: timer:tref(),
                log_buffer = [],
                dropped=0 :: non_neg_integer(),
                send_eol :: boolean()}).


%% API

start_link(Name, Cmd, Args, Opts) ->
    gen_server:start_link(?MODULE,
                          {Name, Cmd, Args, Opts}, []).

init({Name, _Cmd, _Args, Opts} = Params) ->
    process_flag(trap_exit, true), % Make sure terminate gets called
    {SendEOL, Params2} =
        case proplists:get_value(port_server_send_eol, Opts) of
            undefined ->
                {false, Params};
            V ->
                {V, setelement(4, Params, proplists:delete(port_server_send_eol, Opts))}
        end,
    Port = open_port(Params2),
    {ok, #state{port = Port, name = Name, send_eol = SendEOL,
                messages = ringbuffer:new(?NUM_MESSAGES)}}.

handle_info({_Port, {data, {_, Msg}}}, State) ->
    %% Store the last messages in case of a crash
    Messages = ringbuffer:add(Msg, State#state.messages),
    {Buf, Dropped} = case {State#state.log_buffer, State#state.dropped} of
                         {B, D} when length(B) < ?MAX_MESSAGES ->
                             {[Msg|B], D};
                         {B, D} ->
                             {B, D + 1}
                     end,
    TRef = case State#state.log_tref of
               undefined ->
                   timer:send_after(?INTERVAL, log);
               T ->
                   T
           end,
    {noreply, State#state{messages=Messages, log_buffer=Buf, log_tref=TRef,
                          dropped=Dropped}};
handle_info(log, State) ->
    State1 = log(State),
    {noreply, State1};
handle_info({_Port, {exit_status, 0}}, State) ->
    {stop, normal, State};
handle_info({_Port, {exit_status, Status}}, State) ->
    ns_log:log(?MODULE, ?ABNORMAL,
               "Port server ~p on node ~p exited with status ~p. Restarting. "
               "Messages: ~s",
               [State#state.name, node(), Status,
                string:join(ringbuffer:to_list(State#state.messages), "\n")]),
    {stop, {abnormal, Status}, State}.

handle_call(unhandled, unhandled, unhandled) ->
    unhandled.

handle_cast(unhandled, unhandled) ->
    unhandled.

wait_for_child_death(State) ->
    receive
        {Port, _} = Msg when Port =:= State#state.port ->
            wait_for_child_death_process_info(Msg, State);
        log = Msg ->
            wait_for_child_death_process_info(Msg, State);
        X ->
            ?log_error("Ignoring unknown message while shutting down child: ~p~n", [X]),
            wait_for_child_death(State)
    end.

wait_for_child_death_process_info(Msg, State) ->
    case handle_info(Msg, State) of
        {noreply, State2} -> wait_for_child_death(State2);
        {stop, _, State2} -> State2
    end.

terminate(shutdown, #state{send_eol = true, port = Port} = State) ->
    port_command(Port, <<10:8>>),               % sending LF
    timer:send_after(?LF_DEATH_TIMEOUT, {[], {exit_status, -1}}),
    State2 = wait_for_child_death(State),
    log(State2); % Log any remaining messages
terminate(_Reason, State) ->
    log(State). % Log any remaining messages


code_change(_OldVsn, State, _Extra) ->
    {ok, State}.


%% Internal functions

%% @doc Fetch up to Max messages from the queue, discarding any more
%% received up to Timeout. The goal is to remove messages from the
%% queue as fast as possible if the port is spamming, avoiding
%% spamming the log server.
format_lines(Name, Lines) ->
    Prefix = io_lib:format("~p~p: ", [Name, self()]),
    [[Prefix, Line, $\n] || Line <- Lines].


log(State) ->
    case State#state.log_buffer of
        [] ->
            ok;
        Buf ->
            error_logger:info_msg(format_lines(State#state.name,
                                               lists:reverse(Buf))),
            case State#state.dropped of
                0 ->
                    ok;
                Dropped ->
                    ?log_warning("Dropped ~p log lines from ~p",
                                 [Dropped, State#state.name])
            end
    end,
    State#state{log_tref=undefined, log_buffer=[], dropped=0}.


open_port({_Name, Cmd, Args, OptsIn}) ->
    %% Incoming options override existing ones (specified in proplists docs)
    Opts0 = OptsIn ++ [{args, Args}, exit_status, {line, 8192},
                       stderr_to_stdout],
    WriteDataArg = proplists:get_value(write_data, Opts0),
    Opts = lists:keydelete(write_data, 1, Opts0),
    Port = open_port({spawn_executable, Cmd}, Opts),
    case WriteDataArg of
        undefined ->
            ok;
        Data ->
            Port ! {self(), {command, Data}}
    end,
    Port.
