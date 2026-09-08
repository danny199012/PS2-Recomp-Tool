add_executable(ee-analyze main.cpp)
target_link_libraries(ee-analyze PRIVATE ee::analysis ee::elf)
