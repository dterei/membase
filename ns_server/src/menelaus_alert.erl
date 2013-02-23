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

-module(menelaus_alert).
-author('Northscale <info@northscale.com>').

-include("ns_log.hrl").
-include("ns_common.hrl").

-include_lib("eunit/include/eunit.hrl").

-ifdef(EUNIT).
-export([test/0]).
-endif.

-export([handle_logs/1,
         build_logs/1,
         handle_alerts/1,
         parse_settings_alerts_post/1,
         build_alerts_json/1]).

-export([get_alert_config/0,
         set_alert_config/1,
         alert_key/2,
         category_bin/1]).

-import(menelaus_util,
        [reply_json/2]).

%% External API

-define(DEFAULT_LIMIT, 250).

-record(alerts_query_args, {
          %% Whether to enable or disable email notifications
          enabled=false :: true|false,
          %% The sender address of the email
          sender="membase@localhost" :: string(),
          %% A comma separated list of recipients of the of the alert emails.
          recipients=[] :: [string()],
          %% Host address of the SMTP server
          email_host="localhost" :: string(),
          %% Port of the SMTP server
          email_port=25 :: integer(),
          %%  Whether you'd like to use TLS or not
          email_encrypt=false :: true|false,
          %% Username for the SMTP server
          email_user="" :: string(),
          %% Password for the SMTP server
          email_pass="" :: string(),
          %% Comma separated list of alerts that should cause an email to
          %% be sent.
          alerts=[auto_failover_node,
                  auto_failover_maximum_reached,
                  auto_failover_other_nodes_down,
                  auto_failover_cluster_too_small
                 ] :: [atom()]
         }).

handle_logs(Req) ->
    reply_json(Req, {struct, [{list, build_logs(Req:parse_qs())}]}).

handle_alerts(Req) ->
    reply_json(Req, {struct, [{list, build_alerts(Req:parse_qs())}]}).


%% @doc Parse alert setting that were posted. Return either the parsed
%% query or the errors that occured while parsing.
-spec parse_settings_alerts_post(PostArgs::[{string(), string()}]) ->
                                    {ok, [{atom(), any()}]}|
                                    {error, [string()]}.
parse_settings_alerts_post(PostArgs) ->
    QueryParams = lists:foldl(fun({K, V}, Acc) ->
        parse_settings_alerts_param(K, V) ++ Acc
    end, [], PostArgs),
    case QueryParams of
        [] ->
            {error, [{general, <<"No valid parameters given.">>}]};
        QueryParams ->
            {QueryArgs, Errors} =
            lists:foldl(
              fun({K, V}, {Args, Errors}) ->
                  case validate_settings_alerts_query(K, V, Args) of
                      Args2 when is_record(Args2, alerts_query_args) ->
                          {Args2, Errors};
                      Error ->
                          % Save the key with the error, so that they can
                          % be identified for the user interface
                          {Args, [{K, Error}|Errors]}
                  end
              end, {#alerts_query_args{}, []}, lists:reverse(QueryParams)),
            case Errors of
                [] ->
                    Config = build_alerts_config(QueryArgs),
                    {ok, Config};
                Errors ->
                    {error, Errors}
            end
    end.


%%
%% Internal functions
%%

%% @doc Returns a list of all alerts that might send out an email notfication.
%% Every module that creates alerts that should be sent by email needs to
%% implement an alert_keys/0 function that returns all its alert keys.
-spec alert_keys() -> [atom()].
alert_keys() ->
    Modules = [auto_failover],
    Keys = [M:alert_keys() || M <- Modules],
    lists:append(Keys).

%% @doc Create the config structure out of the request parameters.
-spec build_alerts_config(Args::#alerts_query_args{}) -> [{atom(), any()}].
build_alerts_config(Args) ->
     [{recipients, Args#alerts_query_args.recipients},
      {sender, Args#alerts_query_args.sender},
      {enabled, Args#alerts_query_args.enabled},
      {email_server, [{user, Args#alerts_query_args.email_user},
                      {pass, Args#alerts_query_args.email_pass},
                      {host, Args#alerts_query_args.email_host},
                      {port, Args#alerts_query_args.email_port},
                      {encrypt, Args#alerts_query_args.email_encrypt}]},
      {alerts, Args#alerts_query_args.alerts}].



%% @doc Returns the config settings as Mochijson2 JSON
%-spec build_alerts_config(Args::#alerts_query_args{}) -> tuple().
build_alerts_json(Config) ->
    Server = proplists:get_value(email_server, Config),
    Rcpts = [list_to_binary(R) || R <- proplists:get_value(recipients,
                                                           Config)],
    [{recipients, Rcpts},
     {sender, list_to_binary(proplists:get_value(sender, Config))},
     {enabled, proplists:get_value(enabled, Config)},
     {emailServer,
      {struct,
       [{user, list_to_binary(proplists:get_value(user, Server))},
        {pass, <<"">>},
        {host, list_to_binary(proplists:get_value(host, Server))},
        {port, proplists:get_value(port, Server)},
        {encrypt, proplists:get_value(encrypt, Server)}]}},
     {alerts, proplists:get_value(alerts, Config)}].
      %{struct, lists:map(fun(X) -> {atom_to_list(X), true} end,
      %                   proplists:get_value(alerts, C, []))}}].


parse_settings_alerts_param("alerts", Alerts) ->
    [{alerts, [list_to_atom(A) || A <- string:tokens(Alerts, ",")]}];
parse_settings_alerts_param("emailEncrypt", "false") ->
    [{email_encrypt, false}];
parse_settings_alerts_param("emailEncrypt", "true") ->
    [{email_encrypt, true}];
parse_settings_alerts_param("emailEncrypt", _) ->
    [{email_encrypt, error}];
parse_settings_alerts_param("emailHost", Host) ->
    [{email_host, misc:trim(Host)}];
parse_settings_alerts_param("emailPort", Port) ->
    [{email_port, try list_to_integer(Port) catch error:badarg -> error end}];
parse_settings_alerts_param("emailPass", Password) ->
    [{email_pass, Password}];
parse_settings_alerts_param("emailUser", User) ->
    [{email_user, misc:trim(User)}];
parse_settings_alerts_param("enabled", "false") ->
    [{enabled, false}];
parse_settings_alerts_param("enabled", "true") ->
    [{enabled, true}];
parse_settings_alerts_param("enabled", _) ->
    [{enabled, error}];
parse_settings_alerts_param("recipients", Rcpts) ->
    [{recipients, [misc:trim(R) || R <- string:tokens(Rcpts, ",")]}];
parse_settings_alerts_param("sender", Sender) ->
    [{sender, misc:trim(Sender)}];
parse_settings_alerts_param(_Key, _Value) ->
    [].

%% @doc Return either the updated record, or in case of an error the
%% error message
-spec validate_settings_alerts_query(Key::atom(), Value::any,
                                     Args::#alerts_query_args{}) ->
                                        #alerts_query_args{}|binary().
validate_settings_alerts_query(alerts, Alerts, Args) ->
    Keys = alert_keys(),
    case [A || A <- Alerts, not lists:member(A, Keys)] of
        [] ->
            Args#alerts_query_args{alerts=Alerts};
        _ ->
            Keys2 = list_to_binary(string:join(
                                     [atom_to_list(K) || K <- Keys], ", ")),
            {alerts, <<"alerts contained invalid keys. Valid keys are: ",
                       Keys2/binary, ".">>}
    end;
validate_settings_alerts_query(email_encrypt, error, _Args) ->
    <<"emailEncrypt must be either true or false.">>;
validate_settings_alerts_query(email_encrypt, Value, Args) ->
    Args#alerts_query_args{email_encrypt=Value};
validate_settings_alerts_query(email_host, Value, Args) ->
    Args#alerts_query_args{email_host=Value};
validate_settings_alerts_query(email_port, Port, Args) ->
    case (Port=/=error) and (Port>0) and (Port<65535) of
        true -> Args#alerts_query_args{email_port=Port};
        false -> <<"emailPort must be a positive integer less than 65536.">>
    end;
validate_settings_alerts_query(email_pass, Value, Args) ->
    Args#alerts_query_args{email_pass=Value};
validate_settings_alerts_query(email_user, Value, Args) ->
    Args#alerts_query_args{email_user=Value};
validate_settings_alerts_query(enabled, error, _Args) ->
    <<"enabled must be either true or false.">>;
validate_settings_alerts_query(enabled, Value, Args) ->
    Args#alerts_query_args{enabled=Value};
validate_settings_alerts_query(recipients, Rcpts, Args) ->
    case [R || R <- Rcpts, not menelaus_util:validate_email_address(R)] of
        [] ->
            Args#alerts_query_args{recipients=Rcpts};
        _ ->
            <<"recipients must be a comma separated list of valid "
              "email addresses.">>
    end;
validate_settings_alerts_query(sender, Sender, Args) ->
    case menelaus_util:validate_email_address(Sender) of
        true -> Args#alerts_query_args{sender=Sender};
        false -> <<"sender must be a valid email address.">>
    end;
% Additional values are simply ignored
validate_settings_alerts_query(_Key, _Value, Args) ->
    Args.


build_logs(Params) ->
    {MinTStamp, Limit} = common_params(Params),
    build_log_structs(ns_log:recent(), MinTStamp, Limit).

build_log_structs(LogEntriesIn, MinTStamp, Limit) ->
    LogEntries = lists:filter(fun(#log_entry{tstamp = TStamp}) ->
                                      misc:time_to_epoch_ms_int(TStamp) > MinTStamp
                              end,
                              LogEntriesIn),
    LogEntries2 = lists:reverse(lists:keysort(#log_entry.tstamp, LogEntries)),
    LogEntries3 = lists:sublist(LogEntries2, Limit),
    LogStructs =
        lists:foldl(
          fun(#log_entry{node = Node,
                         module = Module,
                         code = Code,
                         msg = Msg,
                         args = Args,
                         cat = Cat,
                         tstamp = TStamp}, Acc) ->
                  case catch(io_lib:format(Msg, Args)) of
                      S when is_list(S) ->
                          CodeString = ns_log:code_string(Module, Code),
                          [{struct, [{node, Node},
                                     {type, category_bin(Cat)},
                                     {code, Code},
                                     {module, list_to_binary(atom_to_list(Module))},
                                     {tstamp, misc:time_to_epoch_ms_int(TStamp)},
                                     {shortText, list_to_binary(CodeString)},
                                     {text, list_to_binary(S)}]} | Acc];
                      _ -> Acc
                  end
          end,
          [],
          LogEntries3),
    LogStructs.

category_bin(info) -> <<"info">>;
category_bin(warn) -> <<"warning">>;
category_bin(crit) -> <<"critical">>;
category_bin(_)    -> <<"info">>.

build_alerts(Params) ->
    {MinTStamp, Limit} = common_params(Params),
    AlertConfig = get_alert_config(),
    Alerts = proplists:get_value(alerts, AlertConfig, []),
    LogEntries = ns_log:recent(),
    LogEntries2 = lists:filter(
                    fun(#log_entry{module = Module,
                                   code = Code}) ->
                            lists:member(alert_key(Module, Code), Alerts)
                    end,
                    LogEntries),
    build_log_structs(LogEntries2, MinTStamp, Limit).

% The defined alert_key() responses are...
%
%  server_down
%  server_unresponsive
%  server_up
%  server_joined
%  server_left
%  bucket_created
%  bucket_deleted
%  bucket_auth_failed
%
% auto_failover_node
% auto_failover_maximum_reached
% auto_failover_other_nodes_down
% auto_failover_cluster_too_small

alert_key(ns_node_disco, 0005) -> server_down;
alert_key(ns_node_disco, 0014) -> server_unresponsive;
alert_key(ns_node_disco, 0004) -> server_up;
alert_key(mc_pool, 0006)       -> bucket_auth_failed;
alert_key(menelaus_web, Code) -> menelaus_web:alert_key(Code);
alert_key(ns_cluster, Code) -> ns_cluster:alert_key(Code);
alert_key(auto_failover, Code) -> auto_failover:alert_key(Code);
alert_key(_Module, _Code) -> all.

get_alert_config() ->
    {value, X} = ns_config:search(ns_config:get(), email_alerts),
    X.

set_alert_config(AlertConfig) ->
    ns_config:set(email_alerts, AlertConfig).

common_params(Params) ->
    MinTStamp = case proplists:get_value("sinceTime", Params) of
                     undefined -> 0;
                     V -> list_to_integer(V)
                 end,
    Limit = case proplists:get_value("limit", Params) of
                undefined -> ?DEFAULT_LIMIT;
                L -> list_to_integer(L)
            end,
    {MinTStamp, Limit}.

-ifdef(EUNIT).

test() ->
    eunit:test({module, ?MODULE},
               [verbose]).

-endif.

