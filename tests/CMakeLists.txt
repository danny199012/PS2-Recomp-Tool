add_executable(test_r5900 test_r5900.cpp)
target_link_libraries(test_r5900 PRIVATE ee::r5900)
add_test(NAME r5900 COMMAND test_r5900)

add_executable(test_elf test_elf.cpp)
target_link_libraries(test_elf PRIVATE ee::elf)
add_test(NAME elf COMMAND test_elf)
