#
# phastf
#
project(phastf Fortran)

# phastf srcs
SET(phastf_SOURCES
PhastFortran/XP_aplbce.f90
PhastFortran/XP_aplbci.F90
PhastFortran/XP_asembl.f90
PhastFortran/XP_asmslc.F90
PhastFortran/XP_crsdsp.f90
PhastFortran/XP_etom2.f90
PhastFortran/XP_module.F90
PhastFortran/XP_rhsn.f90
PhastFortran/XP_sumcal1.f90
PhastFortran/XP_wbcflo.f90
PhastFortran/XP_wellsc.f90
PhastFortran/XP_wellsr.f90
PhastFortran/aplbce_flow.f90
PhastFortran/aplbci_flow.F90
PhastFortran/asembl_flow.f90
PhastFortran/asmslp_flow.F90
PhastFortran/calc_velocity.f90
PhastFortran/calcc.f90
PhastFortran/clog.f90
PhastFortran/closef.F90
PhastFortran/coeff_flow.F90
PhastFortran/coeff_trans.F90
PhastFortran/create_mapping.f90
PhastFortran/dealloc_arr.F90
PhastFortran/dealloc_arr_worker.F90
PhastFortran/dump.f90
PhastFortran/error1.f90
PhastFortran/error2.f90
PhastFortran/error3.f90
PhastFortran/error4.f90
PhastFortran/errprt.f90
PhastFortran/etom1.f90
PhastFortran/etom2.f90
PhastFortran/hdf_f.F90
PhastFortran/ifrd.inc
PhastFortran/ifwr.inc
PhastFortran/incidx.f90
PhastFortran/indx_rewi.f90
PhastFortran/indx_rewi_bc.f90
PhastFortran/init1.F90
PhastFortran/init2_1.F90
PhastFortran/init2_1_xfer.F90
PhastFortran/init2_2.f90
PhastFortran/init2_3.f90
PhastFortran/init2_post_ss.F90
PhastFortran/init3.F90
PhastFortran/init3_xfer.F90
PhastFortran/interpolate_mod.f90
PhastFortran/ld_seg_mod.f90
PhastFortran/ldchar.f90
PhastFortran/ldchar_bc.f90
PhastFortran/mach_mod.f90
PhastFortran/modules_g.f90
PhastFortran/modules_manager.f90
PhastFortran/mpi_mod.F90
PhastFortran/mtoijk.f90
PhastFortran/nintrp.f90
PhastFortran/openf.F90
PhastFortran/phast_manager.F90
PhastFortran/phast_spmd.F90
PhastFortran/phast_worker.F90
PhastFortran/prchar.f90
PhastFortran/print_control_mod.f90
PhastFortran/printar.f90
PhastFortran/read1.F90
PhastFortran/read1_xfer.F90
PhastFortran/read2.F90
PhastFortran/read2_xfer.F90
PhastFortran/read3.F90
PhastFortran/read3_xfer.F90
PhastFortran/reorder_mod.f90
PhastFortran/rewi_mod.f90
PhastFortran/rhsn.f90
PhastFortran/rhsn_manager.F90
PhastFortran/rhsn_ss_flow.f90
PhastFortran/sbcflo.f90
PhastFortran/scale_jds_mod.f90
PhastFortran/set_component_map.F90
PhastFortran/simulate_ss_flow.f90
PhastFortran/solver_direct_mod.f90
PhastFortran/solver_iter_mod.f90
PhastFortran/stonb.f90
PhastFortran/sumcal1.F90
PhastFortran/sumcal1_manager.F90
PhastFortran/sumcal2.f90
PhastFortran/sumcal_ss_flow.f90
PhastFortran/terminate_phast.F90
PhastFortran/terminate_phast_worker.F90
PhastFortran/time_step_save.F90
PhastFortran/timestep.F90
PhastFortran/timestep_ss_flow.f90
PhastFortran/timestep_worker.F90
PhastFortran/update_print_flags.f90
PhastFortran/uppercase.f90
PhastFortran/wbbal.f90
PhastFortran/wbcflo.f90
PhastFortran/wellsc.f90
PhastFortran/wellsc_ss_flow.f90
PhastFortran/wellsr.f90
PhastFortran/wellsr_ss_flow.f90
PhastFortran/welris.f90
PhastFortran/wfdydz.f90
PhastFortran/write1.f90
PhastFortran/write2_1.f90
PhastFortran/write2_2.f90
PhastFortran/write3.f90
PhastFortran/write3_ss_flow.f90
PhastFortran/write4.f90
PhastFortran/write5.f90
PhastFortran/write5_ss_flow.f90
PhastFortran/write6.f90
PhastFortran/xfer_to_manager.F90
PhastFortran/xfer_to_workers.F90
PhastFortran/zone_flow.f90
PhastFortran/zone_flow_ss.F90
PhreeqcRM/src/IPhreeqcPhast/IPhreeqc/IPhreeqc_interface.F90
PhreeqcRM/src/RM_interface.F90
)

# phastf target
add_library(phastf STATIC ${phastf_SOURCES})
set_property(TARGET phastf PROPERTY POSITION_INDEPENDENT_CODE TRUE)

# phastf defines
target_compile_definitions(phastf 
  PRIVATE
    $<$<BOOL:${PHAST_BUILD_MPI}>:USE_MPI>
    $<$<BOOL:${PHAST_BUILD_OPENMP}>:USE_OPENMP>
)

# phastf link libraries
target_link_libraries(phastf
  PRIVATE
    $<$<BOOL:${PHAST_BUILD_MPI}>:MPI::MPI_Fortran>
    $<$<BOOL:${PHAST_BUILD_OPENMP}>:OpenMP::OpenMP_Fortran>
)


#
# PhreeqcRM
#
project(PhreeqcRM CXX)

# PhreeqcRM srcs
SET(PhreeqcRM_SOURCES
PhreeqcRM/src/BMI_interface_F.cpp
PhreeqcRM/src/BMI_interface_F.h
PhreeqcRM/src/bmi.hxx
PhreeqcRM/src/BMIPhreeqcRM.cpp
PhreeqcRM/src/BMIPhreeqcRM.h
PhreeqcRM/src/BMIVariant.cpp
PhreeqcRM/src/BMIVariant.h
PhreeqcRM/src/IPhreeqcPhast/IPhreeqc/CSelectedOutput.cpp
PhreeqcRM/src/IPhreeqcPhast/IPhreeqc/CSelectedOutput.hxx
PhreeqcRM/src/IPhreeqcPhast/IPhreeqc/CVar.hxx
PhreeqcRM/src/IPhreeqcPhast/IPhreeqc/Debug.h
PhreeqcRM/src/IPhreeqcPhast/IPhreeqc/ErrorReporter.hxx
PhreeqcRM/src/IPhreeqcPhast/IPhreeqc/IPhreeqc_interface_F.cpp
PhreeqcRM/src/IPhreeqcPhast/IPhreeqc/IPhreeqc.cpp
PhreeqcRM/src/IPhreeqcPhast/IPhreeqc/IPhreeqc.h
PhreeqcRM/src/IPhreeqcPhast/IPhreeqc/IPhreeqc.hpp
PhreeqcRM/src/IPhreeqcPhast/IPhreeqc/IPhreeqcCallbacks.h
PhreeqcRM/src/IPhreeqcPhast/IPhreeqc/IPhreeqcLib.cpp
PhreeqcRM/src/IPhreeqcPhast/IPhreeqc/phreeqcpp/advection.cpp
PhreeqcRM/src/IPhreeqcPhast/IPhreeqc/phreeqcpp/basicsubs.cpp
PhreeqcRM/src/IPhreeqcPhast/IPhreeqc/phreeqcpp/ChartHandler.cpp
PhreeqcRM/src/IPhreeqcPhast/IPhreeqc/phreeqcpp/ChartHandler.h
PhreeqcRM/src/IPhreeqcPhast/IPhreeqc/phreeqcpp/ChartObject.cpp
PhreeqcRM/src/IPhreeqcPhast/IPhreeqc/phreeqcpp/ChartObject.h
PhreeqcRM/src/IPhreeqcPhast/IPhreeqc/phreeqcpp/cl1.cpp
PhreeqcRM/src/IPhreeqcPhast/IPhreeqc/phreeqcpp/common/Parser.cxx
PhreeqcRM/src/IPhreeqcPhast/IPhreeqc/phreeqcpp/common/Parser.h
PhreeqcRM/src/IPhreeqcPhast/IPhreeqc/phreeqcpp/common/PHRQ_base.cxx
PhreeqcRM/src/IPhreeqcPhast/IPhreeqc/phreeqcpp/common/PHRQ_base.h
PhreeqcRM/src/IPhreeqcPhast/IPhreeqc/phreeqcpp/common/PHRQ_io.cpp
PhreeqcRM/src/IPhreeqcPhast/IPhreeqc/phreeqcpp/common/PHRQ_io.h
PhreeqcRM/src/IPhreeqcPhast/IPhreeqc/phreeqcpp/common/phrqtype.h
PhreeqcRM/src/IPhreeqcPhast/IPhreeqc/phreeqcpp/common/Utils.cxx
PhreeqcRM/src/IPhreeqcPhast/IPhreeqc/phreeqcpp/common/Utils.h
PhreeqcRM/src/IPhreeqcPhast/IPhreeqc/phreeqcpp/CurveObject.cpp
PhreeqcRM/src/IPhreeqcPhast/IPhreeqc/phreeqcpp/CurveObject.h
PhreeqcRM/src/IPhreeqcPhast/IPhreeqc/phreeqcpp/cvdense.cpp
PhreeqcRM/src/IPhreeqcPhast/IPhreeqc/phreeqcpp/cvdense.h
PhreeqcRM/src/IPhreeqcPhast/IPhreeqc/phreeqcpp/cvode.cpp
PhreeqcRM/src/IPhreeqcPhast/IPhreeqc/phreeqcpp/cvode.h
PhreeqcRM/src/IPhreeqcPhast/IPhreeqc/phreeqcpp/cxxKinetics.cxx
PhreeqcRM/src/IPhreeqcPhast/IPhreeqc/phreeqcpp/cxxKinetics.h
PhreeqcRM/src/IPhreeqcPhast/IPhreeqc/phreeqcpp/cxxMix.cxx
PhreeqcRM/src/IPhreeqcPhast/IPhreeqc/phreeqcpp/cxxMix.h
PhreeqcRM/src/IPhreeqcPhast/IPhreeqc/phreeqcpp/dense.cpp
PhreeqcRM/src/IPhreeqcPhast/IPhreeqc/phreeqcpp/dense.h
PhreeqcRM/src/IPhreeqcPhast/IPhreeqc/phreeqcpp/Dictionary.cpp
PhreeqcRM/src/IPhreeqcPhast/IPhreeqc/phreeqcpp/Dictionary.h
PhreeqcRM/src/IPhreeqcPhast/IPhreeqc/phreeqcpp/dumper.cpp
PhreeqcRM/src/IPhreeqcPhast/IPhreeqc/phreeqcpp/dumper.h
PhreeqcRM/src/IPhreeqcPhast/IPhreeqc/phreeqcpp/Exchange.cxx
PhreeqcRM/src/IPhreeqcPhast/IPhreeqc/phreeqcpp/Exchange.h
PhreeqcRM/src/IPhreeqcPhast/IPhreeqc/phreeqcpp/ExchComp.cxx
PhreeqcRM/src/IPhreeqcPhast/IPhreeqc/phreeqcpp/ExchComp.h
PhreeqcRM/src/IPhreeqcPhast/IPhreeqc/phreeqcpp/GasComp.cxx
PhreeqcRM/src/IPhreeqcPhast/IPhreeqc/phreeqcpp/GasComp.h
PhreeqcRM/src/IPhreeqcPhast/IPhreeqc/phreeqcpp/gases.cpp
PhreeqcRM/src/IPhreeqcPhast/IPhreeqc/phreeqcpp/GasPhase.cxx
PhreeqcRM/src/IPhreeqcPhast/IPhreeqc/phreeqcpp/GasPhase.h
PhreeqcRM/src/IPhreeqcPhast/IPhreeqc/phreeqcpp/global_structures.h
PhreeqcRM/src/IPhreeqcPhast/IPhreeqc/phreeqcpp/input.cpp
PhreeqcRM/src/IPhreeqcPhast/IPhreeqc/phreeqcpp/integrate.cpp
PhreeqcRM/src/IPhreeqcPhast/IPhreeqc/phreeqcpp/inverse.cpp
PhreeqcRM/src/IPhreeqcPhast/IPhreeqc/phreeqcpp/ISolution.cxx
PhreeqcRM/src/IPhreeqcPhast/IPhreeqc/phreeqcpp/ISolution.h
PhreeqcRM/src/IPhreeqcPhast/IPhreeqc/phreeqcpp/ISolutionComp.cxx
PhreeqcRM/src/IPhreeqcPhast/IPhreeqc/phreeqcpp/ISolutionComp.h
PhreeqcRM/src/IPhreeqcPhast/IPhreeqc/phreeqcpp/isotopes.cpp
PhreeqcRM/src/IPhreeqcPhast/IPhreeqc/phreeqcpp/kinetics.cpp
PhreeqcRM/src/IPhreeqcPhast/IPhreeqc/phreeqcpp/KineticsComp.cxx
PhreeqcRM/src/IPhreeqcPhast/IPhreeqc/phreeqcpp/KineticsComp.h
PhreeqcRM/src/IPhreeqcPhast/IPhreeqc/phreeqcpp/mainsubs.cpp
PhreeqcRM/src/IPhreeqcPhast/IPhreeqc/phreeqcpp/model.cpp
PhreeqcRM/src/IPhreeqcPhast/IPhreeqc/phreeqcpp/NA.h
PhreeqcRM/src/IPhreeqcPhast/IPhreeqc/phreeqcpp/NameDouble.cxx
PhreeqcRM/src/IPhreeqcPhast/IPhreeqc/phreeqcpp/NameDouble.h
PhreeqcRM/src/IPhreeqcPhast/IPhreeqc/phreeqcpp/NumKeyword.cxx
PhreeqcRM/src/IPhreeqcPhast/IPhreeqc/phreeqcpp/NumKeyword.h
PhreeqcRM/src/IPhreeqcPhast/IPhreeqc/phreeqcpp/nvector_serial.cpp
PhreeqcRM/src/IPhreeqcPhast/IPhreeqc/phreeqcpp/nvector_serial.h
PhreeqcRM/src/IPhreeqcPhast/IPhreeqc/phreeqcpp/nvector.cpp
PhreeqcRM/src/IPhreeqcPhast/IPhreeqc/phreeqcpp/nvector.h
PhreeqcRM/src/IPhreeqcPhast/IPhreeqc/phreeqcpp/parse.cpp
PhreeqcRM/src/IPhreeqcPhast/IPhreeqc/phreeqcpp/PBasic.cpp
PhreeqcRM/src/IPhreeqcPhast/IPhreeqc/phreeqcpp/PBasic.h
PhreeqcRM/src/IPhreeqcPhast/IPhreeqc/phreeqcpp/phqalloc.cpp
PhreeqcRM/src/IPhreeqcPhast/IPhreeqc/phreeqcpp/phqalloc.h
PhreeqcRM/src/IPhreeqcPhast/IPhreeqc/phreeqcpp/Phreeqc.cpp
PhreeqcRM/src/IPhreeqcPhast/IPhreeqc/phreeqcpp/Phreeqc.h
PhreeqcRM/src/IPhreeqcPhast/IPhreeqc/phreeqcpp/PhreeqcKeywords/Keywords.cpp
PhreeqcRM/src/IPhreeqcPhast/IPhreeqc/phreeqcpp/PhreeqcKeywords/Keywords.h
PhreeqcRM/src/IPhreeqcPhast/IPhreeqc/phreeqcpp/PHRQ_io_output.cpp
PhreeqcRM/src/IPhreeqcPhast/IPhreeqc/phreeqcpp/pitzer_structures.cpp
PhreeqcRM/src/IPhreeqcPhast/IPhreeqc/phreeqcpp/pitzer.cpp
PhreeqcRM/src/IPhreeqcPhast/IPhreeqc/phreeqcpp/PPassemblage.cxx
PhreeqcRM/src/IPhreeqcPhast/IPhreeqc/phreeqcpp/PPassemblage.h
PhreeqcRM/src/IPhreeqcPhast/IPhreeqc/phreeqcpp/PPassemblageComp.cxx
PhreeqcRM/src/IPhreeqcPhast/IPhreeqc/phreeqcpp/PPassemblageComp.h
PhreeqcRM/src/IPhreeqcPhast/IPhreeqc/phreeqcpp/prep.cpp
PhreeqcRM/src/IPhreeqcPhast/IPhreeqc/phreeqcpp/Pressure.cxx
PhreeqcRM/src/IPhreeqcPhast/IPhreeqc/phreeqcpp/Pressure.h
PhreeqcRM/src/IPhreeqcPhast/IPhreeqc/phreeqcpp/print.cpp
PhreeqcRM/src/IPhreeqcPhast/IPhreeqc/phreeqcpp/Reaction.cxx
PhreeqcRM/src/IPhreeqcPhast/IPhreeqc/phreeqcpp/Reaction.h
PhreeqcRM/src/IPhreeqcPhast/IPhreeqc/phreeqcpp/read.cpp
PhreeqcRM/src/IPhreeqcPhast/IPhreeqc/phreeqcpp/ReadClass.cxx
PhreeqcRM/src/IPhreeqcPhast/IPhreeqc/phreeqcpp/readtr.cpp
PhreeqcRM/src/IPhreeqcPhast/IPhreeqc/phreeqcpp/runner.cpp
PhreeqcRM/src/IPhreeqcPhast/IPhreeqc/phreeqcpp/runner.h
PhreeqcRM/src/IPhreeqcPhast/IPhreeqc/phreeqcpp/SelectedOutput.cpp
PhreeqcRM/src/IPhreeqcPhast/IPhreeqc/phreeqcpp/SelectedOutput.h
PhreeqcRM/src/IPhreeqcPhast/IPhreeqc/phreeqcpp/Serializer.cxx
PhreeqcRM/src/IPhreeqcPhast/IPhreeqc/phreeqcpp/Serializer.h
PhreeqcRM/src/IPhreeqcPhast/IPhreeqc/phreeqcpp/sit.cpp
PhreeqcRM/src/IPhreeqcPhast/IPhreeqc/phreeqcpp/smalldense.cpp
PhreeqcRM/src/IPhreeqcPhast/IPhreeqc/phreeqcpp/smalldense.h
PhreeqcRM/src/IPhreeqcPhast/IPhreeqc/phreeqcpp/Solution.cxx
PhreeqcRM/src/IPhreeqcPhast/IPhreeqc/phreeqcpp/Solution.h
PhreeqcRM/src/IPhreeqcPhast/IPhreeqc/phreeqcpp/SolutionIsotope.cxx
PhreeqcRM/src/IPhreeqcPhast/IPhreeqc/phreeqcpp/SolutionIsotope.h
PhreeqcRM/src/IPhreeqcPhast/IPhreeqc/phreeqcpp/spread.cpp
PhreeqcRM/src/IPhreeqcPhast/IPhreeqc/phreeqcpp/SS.cxx
PhreeqcRM/src/IPhreeqcPhast/IPhreeqc/phreeqcpp/SS.h
PhreeqcRM/src/IPhreeqcPhast/IPhreeqc/phreeqcpp/SSassemblage.cxx
PhreeqcRM/src/IPhreeqcPhast/IPhreeqc/phreeqcpp/SSassemblage.h
PhreeqcRM/src/IPhreeqcPhast/IPhreeqc/phreeqcpp/SScomp.cxx
PhreeqcRM/src/IPhreeqcPhast/IPhreeqc/phreeqcpp/SScomp.h
PhreeqcRM/src/IPhreeqcPhast/IPhreeqc/phreeqcpp/step.cpp
PhreeqcRM/src/IPhreeqcPhast/IPhreeqc/phreeqcpp/StorageBin.cxx
PhreeqcRM/src/IPhreeqcPhast/IPhreeqc/phreeqcpp/StorageBin.h
PhreeqcRM/src/IPhreeqcPhast/IPhreeqc/phreeqcpp/StorageBinList.cpp
PhreeqcRM/src/IPhreeqcPhast/IPhreeqc/phreeqcpp/StorageBinList.h
PhreeqcRM/src/IPhreeqcPhast/IPhreeqc/phreeqcpp/structures.cpp
PhreeqcRM/src/IPhreeqcPhast/IPhreeqc/phreeqcpp/sundialsmath.cpp
PhreeqcRM/src/IPhreeqcPhast/IPhreeqc/phreeqcpp/sundialsmath.h
PhreeqcRM/src/IPhreeqcPhast/IPhreeqc/phreeqcpp/sundialstypes.h
PhreeqcRM/src/IPhreeqcPhast/IPhreeqc/phreeqcpp/Surface.cxx
PhreeqcRM/src/IPhreeqcPhast/IPhreeqc/phreeqcpp/Surface.h
PhreeqcRM/src/IPhreeqcPhast/IPhreeqc/phreeqcpp/SurfaceCharge.cxx
PhreeqcRM/src/IPhreeqcPhast/IPhreeqc/phreeqcpp/SurfaceCharge.h
PhreeqcRM/src/IPhreeqcPhast/IPhreeqc/phreeqcpp/SurfaceComp.cxx
PhreeqcRM/src/IPhreeqcPhast/IPhreeqc/phreeqcpp/SurfaceComp.h
PhreeqcRM/src/IPhreeqcPhast/IPhreeqc/phreeqcpp/System.cxx
PhreeqcRM/src/IPhreeqcPhast/IPhreeqc/phreeqcpp/System.h
PhreeqcRM/src/IPhreeqcPhast/IPhreeqc/phreeqcpp/tally.cpp
PhreeqcRM/src/IPhreeqcPhast/IPhreeqc/phreeqcpp/Temperature.cxx
PhreeqcRM/src/IPhreeqcPhast/IPhreeqc/phreeqcpp/Temperature.h
PhreeqcRM/src/IPhreeqcPhast/IPhreeqc/phreeqcpp/tidy.cpp
PhreeqcRM/src/IPhreeqcPhast/IPhreeqc/phreeqcpp/transport.cpp
PhreeqcRM/src/IPhreeqcPhast/IPhreeqc/phreeqcpp/Use.cpp
PhreeqcRM/src/IPhreeqcPhast/IPhreeqc/phreeqcpp/Use.h
PhreeqcRM/src/IPhreeqcPhast/IPhreeqc/phreeqcpp/UserPunch.cpp
PhreeqcRM/src/IPhreeqcPhast/IPhreeqc/phreeqcpp/UserPunch.h
PhreeqcRM/src/IPhreeqcPhast/IPhreeqc/phreeqcpp/utilities.cpp
PhreeqcRM/src/IPhreeqcPhast/IPhreeqc/thread.h
PhreeqcRM/src/IPhreeqcPhast/IPhreeqc/Var.c
PhreeqcRM/src/IPhreeqcPhast/IPhreeqc/Var.h
PhreeqcRM/src/IPhreeqcPhast/IPhreeqc/Version.h
PhreeqcRM/src/IPhreeqcPhast/IPhreeqcPhast.cxx
PhreeqcRM/src/IPhreeqcPhast/IPhreeqcPhast.h
PhreeqcRM/src/IPhreeqcPhast/IPhreeqcPhastLib.cpp
PhreeqcRM/src/IPhreeqcPhast/IPhreeqcPhastLib.h
PhreeqcRM/src/IrmResult.h
PhreeqcRM/src/PhreeqcRM.cpp
PhreeqcRM/src/PhreeqcRM.h
PhreeqcRM/src/RM_interface_C.cpp
PhreeqcRM/src/RM_interface_C.h
PhreeqcRM/src/RM_interface_F.cpp
PhreeqcRM/src/RM_interface_F.h
PhreeqcRM/src/RMVARS.h
PhreeqcRM/src/VarManager.cpp
PhreeqcRM/src/VarManager.h
PhreeqcRM/src/YAML_interface_F.cpp
PhreeqcRM/src/YAML_interface_F.h
PhreeqcRM/src/YAMLPhreeqcRM.cpp
PhreeqcRM/src/YAMLPhreeqcRM.h
)

# PhreeqcRM target
add_library(PhreeqcRM STATIC ${PhreeqcRM_SOURCES})
set_property(TARGET PhreeqcRM PROPERTY POSITION_INDEPENDENT_CODE TRUE)

# c++11
target_compile_features(PhreeqcRM PUBLIC cxx_std_11)

# PhreeqcRM defines
target_compile_definitions(PhreeqcRM 
  PRIVATE
    SWIG_SHARED_OBJ
    USE_PHRQ_ALLOC
    USE_GZ
    $<$<BOOL:${PHAST_BUILD_MPI}>:USE_MPI>
    $<$<BOOL:${PHAST_BUILD_OPENMP}>:USE_OPENMP>
)

# PhreeqcRM include directories
target_include_directories(PhreeqcRM 
  PRIVATE
    "${ZLIB_INCLUDE_DIRS}"
    "${PROJECT_BINARY_DIR}"
    "${PROJECT_SOURCE_DIR}/PhreeqcRM/src"
    "${PROJECT_SOURCE_DIR}/PhreeqcRM/src/IPhreeqcPhast"
    "${PROJECT_SOURCE_DIR}/PhreeqcRM/src/IPhreeqcPhast/IPhreeqc"
    "${PROJECT_SOURCE_DIR}/PhreeqcRM/src/IPhreeqcPhast/IPhreeqc/phreeqcpp"
    "${PROJECT_SOURCE_DIR}/PhreeqcRM/src/IPhreeqcPhast/IPhreeqc/phreeqcpp/common"
    "${PROJECT_SOURCE_DIR}/PhreeqcRM/src/IPhreeqcPhast/IPhreeqc/phreeqcpp/PhreeqcKeywords"
)

# generate "${PhreeqcRM_BINARY_DIR}/irm_dll_export.h"
# based on how the PhreeqcRM library is built (BUILD_SHARED_LIBS)
include(GenerateExportHeader)
generate_export_header(PhreeqcRM BASE_NAME IRM_DLL)

# PhreeqcRM link libraries
target_link_libraries(PhreeqcRM
  PRIVATE
    $<$<BOOL:${PHAST_BUILD_MPI}>:MPI::MPI_CXX>
    $<$<BOOL:${PHAST_BUILD_OPENMP}>:OpenMP::OpenMP_CXX>
)


#
# phast3
#
project(phast3 CXX)

# phast3 srcs
SET(phast3_SOURCES
FileHandler.cpp
FileHandler.h
KDtree/Cell_Face.h
KDtree/KDtree.cpp
KDtree/KDtree.h
KDtree/Point.cpp
KDtree/Point.h
KDtree/kdtree2.cpp
TM_interface.cpp
TM_interface.h
gzstream.cxx
gzstream.h
hdf.cpp
hdf.h
phast_main.cpp
)

if (MSVC)
  set(phast3_SOURCES ${phast3_SOURCES} phast.rc)
  set(phast3_SOURCES ${phast3_SOURCES} phast_version.h)
  set(phast3_SOURCES ${phast3_SOURCES} resource.h)
endif()

# phast3 target
add_executable(phast3 ${phast3_SOURCES})
set_property(TARGET phast3 PROPERTY POSITION_INDEPENDENT_CODE TRUE)

# phast3 defines
target_compile_definitions(phast3 
  PRIVATE
    USE_GZ
    HDF5_CREATE
    H5Dcreate_vers=1
    H5Gcreate_vers=1
    H5Eset_auto_vers=1
    ${HDF5_DEFINITIONS}
    $<$<BOOL:${PHAST_BUILD_MPI}>:USE_MPI>
    $<$<BOOL:${PHAST_BUILD_OPENMP}>:USE_OPENMP>
)

# phast3 include directories
target_include_directories(phast3 
  PRIVATE
    "${Boost_INCLUDE_DIRS}"
    "${HDF5_INCLUDE_DIRS}"
    "${ZLIB_INCLUDE_DIRS}"
    "${PROJECT_BINARY_DIR}"
    "${PROJECT_SOURCE_DIR}/PhreeqcRM/src"
    "${PROJECT_SOURCE_DIR}/PhreeqcRM/src/IPhreeqcPhast"
    "${PROJECT_SOURCE_DIR}/PhreeqcRM/src/IPhreeqcPhast/IPhreeqc"
    "${PROJECT_SOURCE_DIR}/PhreeqcRM/src/IPhreeqcPhast/IPhreeqc/phreeqcpp"
    "${PROJECT_SOURCE_DIR}/PhreeqcRM/src/IPhreeqcPhast/IPhreeqc/phreeqcpp/common"
    "${PROJECT_SOURCE_DIR}/PhreeqcRM/src/IPhreeqcPhast/IPhreeqc/phreeqcpp/PhreeqcKeywords"
)

# phast3 link libraries
target_link_libraries(phast3
  PRIVATE
    phastf
    PhreeqcRM
    ${ZLIB_LIBRARIES}
    ${HDF5_LIBRARIES}
    $<$<BOOL:${PHAST_BUILD_MPI}>:MPI::MPI_CXX>
    $<$<BOOL:${PHAST_BUILD_MPI}>:MPI::MPI_Fortran>
    $<$<BOOL:${PHAST_BUILD_OPENMP}>:OpenMP::OpenMP_CXX>
)

# install
install(TARGETS phast3 DESTINATION ${CMAKE_INSTALL_BINDIR})
if(WIN32 AND HDF5_INCLUDE_DIR)
  set(HDF5_DLL "${HDF5_INCLUDE_DIR}/../bin/hdf5.dll")
  if(EXISTS "${HDF5_DLL}")
    install(FILES "${HDF5_DLL}"
            DESTINATION ${CMAKE_INSTALL_BINDIR})
  endif()
  set(SZIP_DLL "${HDF5_INCLUDE_DIR}/../bin/szip.dll")
  if(EXISTS "${SZIP_DLL}")
    install(FILES "${SZIP_DLL}"
            DESTINATION ${CMAKE_INSTALL_BINDIR})
  endif()
  set(ZLIB_DLL "${HDF5_INCLUDE_DIR}/../bin/zlib.dll")
  if(EXISTS "${ZLIB_DLL}")
    install(FILES "${ZLIB_DLL}"
            DESTINATION ${CMAKE_INSTALL_BINDIR})
  endif()
endif()

if(NOT DEFINED CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS_NO_WARNINGS)
  set(CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS_NO_WARNINGS ON)
endif()
include(InstallRequiredSystemLibraries)

