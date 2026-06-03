add_library(coop_rudp STATIC
  rudp.c
)
target_include_directories(coop_rudp PUBLIC ${CMAKE_SOURCE_DIR})
