#
## Tools beside the core: update-feed writes signed update feeds, for the tests and
## for whoever signs a release.
## Copyright (c) 2026, Jan Vidar Krey
#

add_executable(update-feed update-feed.cpp)
target_link_libraries(update-feed PRIVATE quickdc::quickdc)
set_target_properties(update-feed PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF)
quickdc_harden(update-feed)
