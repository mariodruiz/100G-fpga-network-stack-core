open_project TOE_hls_prj

set_top toe

add_files toe_src/ack_delay/ack_delay.cpp
add_files toe_src/close_timer/close_timer.cpp
add_files toe_src/event_engine/event_engine.cpp
add_files toe_src/port_table/port_table.cpp
add_files toe_src/probe_timer/probe_timer.cpp
add_files toe_src/retransmit_timer/retransmit_timer.cpp
add_files toe_src/rx_app_stream_if/rx_app_stream_if.cpp
add_files toe_src/rx_engine/rx_engine.cpp
add_files toe_src/rx_sar_table/rx_sar_table.cpp
add_files toe_src/session_lookup_controller/session_lookup_controller.cpp
add_files toe_src/state_table/state_table.cpp
add_files toe_src/toe.cpp
add_files toe_src/common_utilities/common_utilities.cpp
add_files toe_src/memory_access/memory_access.cpp
add_files toe_src/tx_app_if/tx_app_if.cpp
add_files toe_src/tx_app_interface/tx_app_interface.cpp
add_files toe_src/tx_app_stream_if/tx_app_stream_if.cpp
add_files toe_src/tx_engine/tx_engine.cpp
add_files toe_src/tx_sar_table/tx_sar_table.cpp

add_files -tb echo_server_src/echo_server_application.cpp
add_files -tb iperf_client_src/iperf_client.cpp
add_files -tb toe_src/testbench/dummy_memory.cpp
add_files -tb toe_src/testbench/pcap.cpp
add_files -tb toe_src/testbench/pcap2stream.cpp
add_files -tb toe_src/testbench/test_toe.cpp

open_solution "ultrascale_plus"
set_part {xcvu9p-flga2104-2l-e} -tool vivado
create_clock -period 3.1 -name default
set_clock_uncertainty 0.2

csynth_design
export_design -rtl verilog -format ip_catalog
#
exit