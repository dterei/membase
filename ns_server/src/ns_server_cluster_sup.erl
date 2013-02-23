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
-module(ns_server_cluster_sup).

-behavior(supervisor).

%% API
-export ([start_cluster/0, start_link/0, stop_cluster/0]).

%% Supervisor callbacks
-export([init/1]).

%%
%% API
%%

%% @doc Start child after its been stopped
start_cluster() ->
    supervisor:restart_child(?MODULE, ns_server_sup).


%% @doc Start the supervisor
start_link() ->
    supervisor:start_link({local, ?MODULE}, ?MODULE, []).


%% @doc Stop ns_server_sup
stop_cluster() ->
    supervisor:terminate_child(?MODULE, ns_server_sup).


%%
%% Supervisor callbacks
%%

init([]) ->
    {ok, {{one_for_one, 10, 1},
          [{ns_log_mf_h, {ns_log_mf_h, start_link, []},
            permanent, 1000, worker, [ns_log_mf_h]},
           {log_os_info, {log_os_info, start_link, []},
            transient, 1000, worker, [log_os_info]},
           {timeout_diag_logger, {timeout_diag_logger, start_link, []},
            permanent, 1000, worker, [timeout_diag_logger, diag_handler]},
           {dist_manager, {dist_manager, start_link, []},
            permanent, 1000, worker, [dist_manager]},
           {ns_cluster, {ns_cluster, start_link, []},
            permanent, 5000, worker, [ns_cluster]},
           {mb_mnesia_sup, {mb_mnesia_sup, start_link, []},
            permanent, infinity, supervisor, [mb_mnesia_sup]},
           {ns_config_sup, {ns_config_sup, start_link, []},
            permanent, infinity, supervisor,
            [ns_config_sup]},
           {ns_server_sup, {ns_server_sup, start_link, []},
            permanent, infinity, supervisor, [ns_server_sup]}
          ]}}.
