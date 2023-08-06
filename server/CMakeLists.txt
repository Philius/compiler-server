#
# Dependencies
#
find_package(Threads REQUIRED)
find_package(absl REQUIRED)

#
# Sources
#
set(SOURCES
    src/main.cpp
)

source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES ${SOURCES})


#
# Target
#
add_executable(server ${SOURCES}
    src/cyan-semaphore.h)
target_link_libraries(server
    PRIVATE
        csproto
        absl::flags_parse
)
