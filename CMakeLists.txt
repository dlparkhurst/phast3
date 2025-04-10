cmake_minimum_required(VERSION 3.16...3.26)

project(phast CXX C Fortran)

# set version numbers
set(PHAST_VERSION_MAJOR "3")
set(PHAST_VERSION_MINOR "0")
set(PHAST_VERSION_PATCH "0")
set(PHAST_REVISION "9000")

include(CheckPIESupported)
check_pie_supported()

# set default windows installation path
if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT AND WIN32)
  GetDefaultWindowsPrefixBase(CMAKE_GENERIC_PROGRAM_FILES)
  set(W_PREFIX)
  string(APPEND W_PREFIX "${CMAKE_GENERIC_PROGRAM_FILES}/USGS/${PROJECT_NAME}-")
  string(APPEND W_PREFIX "${PHAST_VERSION_MAJOR}.${PHAST_VERSION_MINOR}.")
  string(APPEND W_PREFIX "${PHAST_VERSION_PATCH}-${PHAST_REVISION}")
  set(CMAKE_INSTALL_PREFIX ${W_PREFIX}
      CACHE PATH "Install path prefix, prepended onto install directories."
            FORCE)
endif()

include(GNUInstallDirs)

# overide docdir on windows
if(WIN32 AND NOT CMAKE_INSTALL_DOCDIR)
  set(CMAKE_INSTALL_DOCDIR "" CACHE PATH "documentation root (doc)")
  set(CMAKE_INSTALL_DOCDIR "doc")
endif()

# check for OpenMP
option(PHAST_BUILD_OPENMP "Build OpenMP configuration" "OFF")
if(PHAST_BUILD_OPENMP)
  find_package(OpenMP REQUIRED) 
endif()

# check for Boost
# set BOOST_ROOT or CMAKE_PREFIX_PATH using environment or cache variable
# ie 
#   set CMAKE_PREFIX_PATH=D:/lib/boost_1_76_0
#   cmake ..
# or
#   cmake -DCMAKE_PREFIX_PATH:PATH=D:/lib/boost_1_76_0 ..
find_package(Boost 1.33 REQUIRED)

# check for ZLIB
find_package(ZLIB REQUIRED)

# check for HDF5
# set HDF5_ROOT or CMAKE_PREFIX_PATH using environment or cache variable
# ie 
#   set CMAKE_PREFIX_PATH=D:/lib/boost_1_76_0;C:/Program Files/HDF_Group/HDF5/1.12.0
#   cmake ..
# or
#   cmake -D "CMAKE_PREFIX_PATH:PATH=D:/lib/boost_1_76_0;C:/Program Files/HDF_Group/HDF5/1.12.0" ..
# set(HDF5_FIND_DEBUG TRUE)
find_package(HDF5 REQUIRED)

# check for MPI
option(PHAST_BUILD_MPI "Build MPI configuration (Overrides OpenMP option)"
       "OFF")
if(PHAST_BUILD_MPI)
  if(DEFINED ENV{MSMPI_INC} AND CMAKE_VERSION VERSION_GREATER_EQUAL 3.17)
    # CMake's FindMPI is broken for MSMPI(Fortran) starting with 3.17
    message(STATUS "Using FindMPI from CMake 3.16 since MSMPI_INC is set")
    set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} "${CMAKE_SOURCE_DIR}/cmake")
  endif()
  if(MSVC)
    # FindMPI sometimes fails without this
    set(MPI_Fortran_LINK_FLAGS ${MPI_Fortran_LINK_FLAGS}
                               "/NODEFAULTLIB:LIBCMT.lib")
  endif()
  # To prioritize Intel's MPI, set the I_MPI_ROOT environment variable
  # ie (set I_MPI_ROOT=C:\Program Files (x86)\Intel\oneAPI\mpi\latest)
  # or ($env:I_MPI_ROOT="C:\Program Files (x86)\Intel\oneAPI\mpi\latest")
  find_package(MPI REQUIRED)
endif()
     
if(PHAST_BUILD_MPI AND PHAST_BUILD_OPENMP)
  message(
    FATAL_ERROR "PHAST_BUILD_MPI and PHAST_BUILD_OPENMP are mutually exclusive."
  )
endif()


if(NOT EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/database/phast.dat)
  file(CREATE_LINK ${CMAKE_CURRENT_SOURCE_DIR}/database/phreeqc.dat
       ${CMAKE_CURRENT_SOURCE_DIR}/database/phast.dat COPY_ON_ERROR)
endif()



# database
SET(phast_DATABASE
  database/Amm.dat
  database/ColdChem.dat
  database/Concrete_PHR.dat
  database/Concrete_PZ.dat
  database/core10.dat
  database/frezchem.dat
  database/iso.dat
  database/Kinec_v3.dat
  database/Kinec.v2.dat
  database/llnl.dat
  database/minteq.dat
  database/minteq.v4.dat
  database/phast.dat
  database/phreeqc_rates.dat
  database/PHREEQC_ThermoddemV1.10_15Dec2020.dat
  database/pitzer.dat
  database/sit.dat
  database/stimela.dat
  database/Tipping_Hurley.dat
  database/wateq4f.dat
)

if(WIN32)
  install(FILES ${phast_DATABASE} DESTINATION database)
else()
  install(FILES ${phast_DATABASE} DESTINATION ${CMAKE_INSTALL_DOCDIR}/database)
endif()

include(CTest)

# subdirs
add_subdirectory(doc)
add_subdirectory(examples)
add_subdirectory(src)
add_subdirectory(test)

include(CPack)
