add_executable(ee-disasm main.cpp)
target_link_libraries(ee-disasm PRIVATE ee::elf ee::r5900)
