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
%% @doc Log information about the OS
%%
-module (log_os_info).

-include("ns_common.hrl").

-export([start_link/0]).

start_link() ->
    ?log_info("OS type: ~p Version: ~p~nRuntime info: ~p",
              [os:type(), os:version(), ns_info:runtime()]),
    ?log_info("Manifest:~n~p~n", [diag_handler:manifest()]),
    ignore.
