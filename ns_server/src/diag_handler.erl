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
%% @doc Implementation of diagnostics web requests
-module(diag_handler).
-author('NorthScale <info@northscale.com>').

-include("ns_common.hrl").
-include("ns_log.hrl").

-export([do_diag_per_node/0, handle_diag/1, handle_sasl_logs/1,
         arm_timeout/2, arm_timeout/1, disarm_timeout/1,
         grab_process_info/1, manifest/0,
         diagnosing_timeouts/1]).

diag_filter_out_config_password_list([], UnchangedMarker) ->
    UnchangedMarker;
diag_filter_out_config_password_list([X | Rest], UnchangedMarker) ->
    NewX = diag_filter_out_config_password_rec(X, UnchangedMarker),
    case diag_filter_out_config_password_list(Rest, UnchangedMarker) of
        UnchangedMarker ->
            case NewX of
                UnchangedMarker -> UnchangedMarker;
                _ -> [NewX | Rest]
            end;
        NewRest -> [case NewX of
                        UnchangedMarker -> X;
                        _ -> NewX
                    end | NewRest]
    end;
%% this handles improper list end
diag_filter_out_config_password_list(X, UnchangedMarker) ->
    diag_filter_out_config_password_list([X], UnchangedMarker).

diag_filter_out_config_password_rec(Config, UnchangedMarker) when is_tuple(Config) ->
    case Config of
        {password, _} ->
            {password, 'filtered-out'};
        {pass, _} ->
            {pass, 'filtered-out'};
        _ -> case diag_filter_out_config_password_rec(tuple_to_list(Config), UnchangedMarker) of
                 UnchangedMarker -> Config;
                 List -> list_to_tuple(List)
             end
    end;
diag_filter_out_config_password_rec(Config, UnchangedMarker) when is_list(Config) ->
    diag_filter_out_config_password_list(Config, UnchangedMarker);
diag_filter_out_config_password_rec(_Config, UnchangedMarker) -> UnchangedMarker.

diag_filter_out_config_password(Config) ->
    UnchangedMarker = make_ref(),
    case diag_filter_out_config_password_rec(Config, UnchangedMarker) of
        UnchangedMarker -> Config;
        NewConfig -> NewConfig
    end.

% Read the manifest.txt file
manifest() ->
    case file:read_file(filename:join(path_config:component_path(bin, ".."), "manifest.txt")) of
        {ok, C} ->
            lists:sort(string:tokens(binary_to_list(C), "\n"));
        _ -> []
    end.

grab_process_info(Pid) ->
    PureInfo = erlang:process_info(Pid,
                                   [registered_name,
                                    status,
                                    initial_call,
                                    backtrace,
                                    error_handler,
                                    garbage_collection,
                                    heap_size,
                                    total_heap_size,
                                    links,
                                    memory,
                                    message_queue_len,
                                    reductions,
                                    trap_exit]),
    Backtrace = proplists:get_value(backtrace, PureInfo),
    NewBacktrace = [case erlang:size(X) of
                        L when L =< 90 ->
                            X;
                        _ -> binary:part(X, 1, 90)
                    end || X <- binary:split(Backtrace, <<"\n">>, [global])],
    lists:keyreplace(backtrace, 1, PureInfo, {backtrace, NewBacktrace}).

do_diag_per_node() ->
    [{version, ns_info:version()},
     {manifest, manifest()},
     {config, diag_filter_out_config_password(ns_config:get_kv_list())},
     {basic_info, element(2, ns_info:basic_info())},
     {processes, [{Pid, (catch grab_process_info(Pid))}
                  || Pid <- erlang:processes()]},
     {memory, memsup:get_memory_data()},
     {disk, disksup:get_disk_data()}].

diag_multicall(Mod, F, Args) ->
    Nodes = [node() | nodes()],
    {Results, BadNodes} = rpc:multicall(Nodes, Mod, F, Args, 5000),
    lists:zipwith(fun (Node,Res) -> {Node, Res} end,
                  lists:subtract(Nodes, BadNodes),
                  Results)
        ++ lists:map(fun (Node) -> {Node, diag_failed} end,
                     BadNodes).

diag_format_timestamp(EpochMilliseconds) ->
    SecondsRaw = trunc(EpochMilliseconds/1000),
    AsNow = {SecondsRaw div 1000000, SecondsRaw rem 1000000, 0},
    %% not sure about local time here
    {{YYYY, MM, DD}, {Hour, Min, Sec}} = calendar:now_to_local_time(AsNow),
    io_lib:format("~4.4.0w-~2.2.0w-~2.2.0w ~2.2.0w:~2.2.0w:~2.2.0w.~3.3.0w",
                  [YYYY, MM, DD, Hour, Min, Sec, EpochMilliseconds rem 1000]).

generate_diag_filename() ->
    {{YYYY, MM, DD}, {Hour, Min, Sec}} = calendar:now_to_local_time(now()),
    io_lib:format("ns-diag-~4.4.0w~2.2.0w~2.2.0w~2.2.0w~2.2.0w~2.2.0w.txt",
                  [YYYY, MM, DD, Hour, Min, Sec]).

diag_format_log_entry(Type, Code, Module, Node, TStamp, ShortText, Text) ->
    FormattedTStamp = diag_format_timestamp(TStamp),
    io_lib:format("~s ~s:~B:~s:~s(~s) - ~s~n",
                  [FormattedTStamp, Module, Code, Type, ShortText, Node, Text]).

get_logs() ->
    try ns_log_browser:get_logs_as_file(all, all, []) of
        X -> X
    catch
        error:try_again ->
            timer:sleep(250),
            get_logs()
    end.

handle_diag(Req) ->
    Buckets = lists:sort(fun (A,B) -> element(1, A) =< element(1, B) end,
                         ns_bucket:get_buckets()),
    Infos = [["per_node_diag = ~p", diag_multicall(?MODULE, do_diag_per_node, [])],
             ["nodes_info = ~p", menelaus_web:build_nodes_info()],
             ["buckets = ~p", Buckets],
             ["logs:~n-------------------------------~n"]],
    Text = lists:flatmap(fun ([Fmt | Args]) ->
                                 io_lib:format(Fmt ++ "~n~n", Args)
                         end, Infos),
    Params = Req:parse_qs(),
    MaybeContDisp = case proplists:get_value("mode", Params) of
                        "view" -> [];
                        _ -> [{"Content-Disposition", "attachment; filename=" ++ generate_diag_filename()}]
                    end,
    Resp = Req:ok({"text/plain; charset=utf-8",
                   MaybeContDisp ++
                       [{"Content-Type", "text/plain"}
                        | menelaus_util:server_header()],
                   chunked}),
    Resp:write_chunk(list_to_binary(Text)),
    lists:foreach(fun (#log_entry{node = Node,
                                  module = Module,
                                  code = Code,
                                  msg = Msg,
                                  args = Args,
                                  cat = Cat,
                                  tstamp = TStamp}) ->
                          try io_lib:format(Msg, Args) of
                              S ->
                                  CodeString = ns_log:code_string(Module, Code),
                                  Type = menelaus_alert:category_bin(Cat),
                                  TStampEpoch = misc:time_to_epoch_ms_int(TStamp),
                                  Resp:write_chunk(iolist_to_binary(diag_format_log_entry(Type, Code, Module, Node, TStampEpoch, CodeString, S)))
                          catch _:_ -> ok
                          end
                  end, lists:keysort(#log_entry.tstamp, ns_log:recent())),
    Resp:write_chunk(<<"logs_node:\n-------------------------------\n">>),
    handle_logs(Resp).

handle_logs(Resp) ->
    TempFile = get_logs(),
    {ok, IO} = file:open(TempFile, [raw, binary]),
    stream_logs(Resp, IO),
    ok = file:close(IO),
    file:delete(TempFile).

stream_logs(Resp, IO) ->
    case file:read(IO, 65536) of
        eof ->
            Resp:write_chunk(<<"">>),
            ok;
        {ok, Data} ->
            Resp:write_chunk(Data),
            stream_logs(Resp, IO)
    end.

handle_sasl_logs(Req) ->
    Resp = Req:ok({"text/plain; charset=utf-8",
            menelaus_util:server_header(),
            chunked}),
    handle_logs(Resp).

arm_timeout(Millis) ->
    arm_timeout(Millis,
                fun (Pid) ->
                        Info = grab_process_info(Pid),
                        io:format("slow process info:~n~p~n", [Info])
                end).

arm_timeout(Millis, Callback) ->
    Pid = self(),
    spawn_link(fun () ->
                       receive
                           done -> ok
                       after Millis ->
                                 Callback(Pid)
                       end
               end).

disarm_timeout(Pid) ->
    Pid ! done.

diagnosing_timeouts(Body) ->
    try Body()
    catch exit:{timeout, _} = X ->
            timeout_diag_logger:log_diagnostics(X),
            exit(X)
    end.
