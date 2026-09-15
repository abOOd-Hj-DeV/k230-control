# PC-only: both cores in one process, wired with in-process queues instead of
# DATAFIFO/IPCMSG. This is the development and debugging harness.
add_executable(k230-monitor monitor.cpp)
target_link_libraries(k230-monitor PRIVATE k230_bridge k230_inspector)
