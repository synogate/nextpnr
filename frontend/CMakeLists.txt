add_library(nextpnr_frontend INTERFACE)

target_include_directories(nextpnr_frontend INTERFACE .)

target_sources(nextpnr_frontend INTERFACE
    auto_names.cc
    auto_names.h
    frontend_base.h
    json_frontend.cc
    json_frontend.h
)
