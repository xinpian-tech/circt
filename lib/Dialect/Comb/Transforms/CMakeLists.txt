add_circt_dialect_library(CIRCTCombTransforms
  LowerComb.cpp
  IntRangeOptimizations.cpp
  IntRangeAnnotations.cpp
  BalanceMux.cpp
  AssumeTwoValued.cpp
  SimplifyTruthTable.cpp
  UnshareOperators.cpp
  MuxDontCare.cpp
  SplitCycles.cpp

  DEPENDS
  CIRCTCombTransformsIncGen

  LINK_LIBS PUBLIC
  CIRCTHW
  CIRCTSV
  CIRCTComb
  CIRCTSupport
  MLIRIR
  MLIRPass
  MLIRTransformUtils
)
