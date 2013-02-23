%% @author Northscale <info@northscale.com>
%% @copyright 2010 NorthScale, Inc.
%% All rights reserved.

-module(addr_util).

-include_lib("eunit/include/eunit.hrl").

-export([get_my_address/0, get_my_address/1]).

%% Find the best IP address we can find for the current host.
get_my_address() ->
    get_my_address("255.255.255.255").

get_my_address(CurrentAddress) ->
    determine_address(CurrentAddress, list_all_addresses()).

get_my_address_test() ->
    %% Verify the result of this looks like an IP address.
    4 = length(string:tokens(get_my_address(), ".")).

determine_address(CurrentAddress, AddrList) ->
    case lists:member(CurrentAddress, AddrList) of
        true -> CurrentAddress;
        _ -> extract_addr(AddrList)
    end.

determine_address_test() ->
    "4.1.1.1" = determine_address("255.255.255.255", ["4.1.1.1", "1.2.3.4"]),
    "1.2.3.4" = determine_address("1.2.3.4", ["4.1.1.1", "1.2.3.4"]),
    "127.0.0.1" = determine_address("1.2.3.4", []).

list_all_addresses() ->
    {ok, AddrInfo} = inet:getifaddrs(),
    CandidateList = lists:map(fun(X) -> element(2, X) end,
                              lists:filter(fun(Y) -> element(1, Y) == addr end,
                                           lists:flatten(lists:map(fun(Z) -> element(2, Z) end,
                                                         AddrInfo)))),
    lists:sort(lists:map(fun(X) -> addr_to_s(X) end,
                         lists:filter(fun(Y) -> size(Y) == 4 end,
                                      CandidateList))
               -- ["127.0.0.1"]).

list_all_addresses_test() ->
    %% I can't test too much here since this isn't functional, but
    %% I'll verify we go through the code and return something.
    true = is_list(list_all_addresses()).

%% [X,...] -> X
extract_addr([H|_Tl]) ->
    H;
%% [] -> "127.0.0.1"
extract_addr([]) ->
    "127.0.0.1".

extract_addr_test() ->
    "4.1.1.1" = extract_addr(["4.1.1.1", "1.2.3.4"]),
    "4.1.1.1" = extract_addr(["4.1.1.1"]),
    "127.0.0.1" = extract_addr([]).

%% {1,2,3,4} -> "1.2.3.4"
addr_to_s(T) ->
    string:join(lists:map(fun erlang:integer_to_list/1,
                          tuple_to_list(T)),
                ".").

addr_to_s_test() ->
    "1.2.3.4" = addr_to_s({1,2,3,4}).
