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
%% @doc Web server for menelaus.

-module(menelaus_util).
-author('Northscale <info@northscale.com>').

-include_lib("eunit/include/eunit.hrl").

-ifdef(EUNIT).
-export([test_under_debugger/0, debugger_apply/2]).
-endif.

-export([server_header/0,
         redirect_permanently/2,
         redirect_permanently/3,
         reply_json/2,
         reply_json/3,
         parse_json/1,
         parse_boolean/1,
         expect_config/1,
         get_option/2,
         local_addr/1,
         concat_url_path/1,
         parse_validate_number/3,
         validate_email_address/1,
         insecure_pipe_through_command/2]).

-export([java_date/0,
         string_hash/1,
         my_seed/1]).

-export([stateful_map/3,
         stateful_takewhile/3,
         low_pass_filter/2]).

%% External API

server_header() ->
    Versions = ns_info:version(),
    ServerHeader = lists:concat([
                       "Membase Server ", proplists:get_value(ns_server, Versions)]),
    [{"Pragma", "no-cache"},
     {"Cache-Control", "no-cache"},
     {"Server", ServerHeader}].

redirect_permanently(Path, Req) -> redirect_permanently(Path, Req, []).

%% mostly extracted from mochiweb_request:maybe_redirect/3
redirect_permanently(Path, Req, ExtraHeaders) ->
    %% TODO: support https transparently
    Location =
        case Req:get_header_value("host") of
            undefined -> Path;
            X -> "http://" ++ X ++ Path
        end,
    LocationBin = list_to_binary(Location),
    Top = <<"<!DOCTYPE HTML PUBLIC \"-//IETF//DTD HTML 2.0//EN\">"
           "<html><head>"
           "<title>301 Moved Permanently</title>"
           "</head><body>"
           "<h1>Moved Permanently</h1>"
           "<p>The document has moved <a href=\"">>,
    Bottom = <<">here</a>.</p></body></html>\n">>,
    Body = <<Top/binary, LocationBin/binary, Bottom/binary>>,
    Req:respond({301,
                 [{"Location", Location},
                  {"Content-Type", "text/html"} | ExtraHeaders],
                 Body}).

reply_json(Req, Body) ->
    Req:ok({"application/json",
            server_header(),
            mochijson2:encode(Body)}).

reply_json(Req, Body, Status) ->
    Req:respond({Status,
                 [{"Content-Type", "application/json"}
                  | server_header()],
                 mochijson2:encode(Body)}).

expect_config(Key) ->
    {value, RV} = ns_config:search_node(Key),
    RV.

%% milliseconds since 1970 Jan 1 at UTC
java_date() ->
    {MegaSec, Sec, Micros} = erlang:now(),
    (MegaSec * 1000000 + Sec) * 1000 + (Micros div 1000).

string_hash(String) ->
    lists:foldl((fun (Val, Acc) -> (Acc * 31 + Val) band 16#0fffffff end),
                0,
                String).

my_seed(Number) ->
    {Number*31, Number*13, Number*113}.

%% applies F to every InList element and current state.
%% F must return pair of {new list element value, new current state}.
%% returns pair of {new list, current state}
full_stateful_map(F, InState, InList) ->
    {RV, State} = full_stateful_map_rec(F, InState, InList, []),
    {lists:reverse(RV), State}.

full_stateful_map_rec(_F, State, [], Acc) ->
    {Acc, State};
full_stateful_map_rec(F, State, [H|Tail], Acc) ->
    {Value, NewState} = F(H, State),
    full_stateful_map_rec(F, NewState, Tail, [Value|Acc]).

%% same as full_stateful_map/3, but discards state and returns only transformed list
stateful_map(F, InState, InList) ->
    element(1, full_stateful_map(F, InState, InList)).

low_pass_filter(Alpha, List) ->
    Beta = 1 - Alpha,
    F = fun (V, Prev) ->
                RV = Alpha*V + Beta*Prev,
                {RV, RV}
        end,
    case List of
        [] -> [];
        [H|Tail] -> [H | stateful_map(F, H, Tail)]
    end.

-ifdef(EUNIT).

string_hash_test() ->
    ?assertEqual(string_hash("hi"), $h*31+$i).

debugger_apply(Fun, Args) ->
    i:im(),
    {module, _} = i:ii(?MODULE),
    i:iaa([break]),
    ok = i:ib(?MODULE, Fun, length(Args)),
    apply(?MODULE, Fun, Args).

test_under_debugger() ->
    i:im(),
    {module, _} = i:ii(?MODULE),
    i:iaa([init]),
    eunit:test({spawn, {timeout, infinity, {module, ?MODULE}}}, [verbose]).

-endif.

get_option(Option, Options) ->
    {proplists:get_value(Option, Options),
     proplists:delete(Option, Options)}.

stateful_takewhile_rec(_F, [], _State, App) ->
    App;
stateful_takewhile_rec(F, [H|Tail], State, App) ->
    case F(H, State) of
        {true, NewState} ->
            stateful_takewhile_rec(F, Tail, NewState, [H|App]);
        _ -> App
    end.

stateful_takewhile(F, List, State) ->
    lists:reverse(stateful_takewhile_rec(F, List, State, [])).

parse_json(Req) ->
    mochijson2:decode(Req:recv_body()).

parse_boolean(Value) ->
    case Value of
        true -> true;
        false -> false;
        <<"true">> -> true;
        <<"false">> -> false;
        <<"1">> -> true;
        <<"0">> -> false;
        1 -> true;
        0 -> false
    end.

concat_url_path(Segments) ->
    "/" ++ string:join(lists:map(fun mochiweb_util:quote_plus/1, Segments), "/").

-spec parse_validate_number(string(), (integer() | undefined), (integer() | undefined)) ->
                                   invalid | too_small | too_large | {ok, integer()}.
parse_validate_number(String, Min, Max) ->
    Parsed = (catch list_to_integer(string:strip(String))),
    if
        is_integer(Parsed) ->
            if
                Min =/= undefined andalso Parsed < Min -> too_small;
                Max =/= undefined andalso Parsed > Max -> too_large;
                true -> {ok, Parsed}
            end;
       true -> invalid
    end.

%% does a simple email address validation
validate_email_address(Address) ->
    {ok, RE} = re:compile("^[^@]+@.+$", [multiline]), %%" "hm, even erlang-mode is buggy :("),
    RV = re:run(Address, RE),
    case RV of
        {match, _} -> true;
        _ -> false
    end.

%% Extract the local address of the socket used for the request
local_addr(Req) ->
    {ok, {Address, _Port}} = inet:sockname(Req:get(socket)),
    string:join(lists:map(fun integer_to_list/1, tuple_to_list(Address)), ".").

pipe_through_command_rec(Port, Acc) ->
    receive
        {Port, {data, Data}} ->
            pipe_through_command_rec(Port, [Data | Acc]);
        {Port, {exit_status, _}} ->
            lists:reverse(Acc);
        X when is_tuple(X) andalso element(1, X) =:= Port ->
            io:format("ignoring port message: ~p~n", [X]),
            pipe_through_command_rec(Port, Acc)
    end.

%% this is NOT secure, because I cannot make erlang ports work as
%% popen. We're missing ability to close write side of the port.
insecure_pipe_through_command(Command, IOList) ->
    TmpFile = filename:join(path_config:component_path(tmp),
                            "pipethrough." ++ integer_to_list(erlang:phash2([self(), os:getpid(), timestamp]))),
    filelib:ensure_dir(TmpFile),
    file:write_file(TmpFile, IOList),
    Port = open_port({spawn, Command ++ " <" ++ mochiweb_util:shell_quote(TmpFile)}, [binary, in, exit_status]),
    RV = pipe_through_command_rec(Port, []),
    file:delete(TmpFile),
    RV.
