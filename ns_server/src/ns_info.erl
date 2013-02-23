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
-module(ns_info).

-export([version/0, version/1, runtime/0, basic_info/0]).

version() ->
    lists:map(fun({App, _, Version}) -> {App, Version} end,
              application:loaded_applications()).

version(App) ->
    proplists:get_value(App, version()).

runtime() ->
    {WallClockMSecs, _} = erlang:statistics(wall_clock),
    [{otp_release, erlang:system_info(otp_release)},
     {erl_version, erlang:system_info(version)},
     {erl_version_long, erlang:system_info(system_version)},
     {system_arch_raw, erlang:system_info(system_architecture)},
     {system_arch, system_arch()},
     {localtime, erlang:localtime()},
     {memory, erlang:memory()},
     {loaded, erlang:loaded()},
     {applications, application:loaded_applications()},
     {pre_loaded, erlang:pre_loaded()},
     {process_count, erlang:system_info(process_count)},
     {node, erlang:node()},
     {nodes, erlang:nodes()},
     {registered, erlang:registered()},
     {cookie, erlang:get_cookie()},
     {wordsize, erlang:system_info(wordsize)},
     {wall_clock, trunc(WallClockMSecs / 1000)}].

basic_info() ->
    {WallClockMSecs, _} = erlang:statistics(wall_clock),
    {erlang:node(),
     [{version, version()},
      {system_arch, system_arch()},
      {wall_clock, trunc(WallClockMSecs / 1000)},
      {memory_data, memsup:get_memory_data()},
      {disk_data, disksup:get_disk_data()}]}.

system_arch() ->
    case erlang:system_info(system_architecture) of
        "win32" ->
            % Per bug 607, erlang R13B03 doesn't know it's on a 64-bit windows,
            % and always reports "win32".  So, just report "windows".
            "windows";
        X -> X
    end.
