add_executable(ee-recomp main.cpp)
target_link_libraries(ee-recomp PRIVATE ee::codegen ee::analysis ee::elf)
