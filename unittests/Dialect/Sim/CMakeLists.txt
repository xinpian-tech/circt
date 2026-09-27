add_circt_unittest(CIRCTSimTests
  ExportDPIInterfaceTest.cpp
)

target_link_libraries(CIRCTSimTests
  PRIVATE
  CIRCTHW
  CIRCTMoore
  CIRCTSV
  CIRCTSim
  CIRCTSimTransforms
  MLIRLLVMDialect
  MLIRIR
  MLIRParser
)
