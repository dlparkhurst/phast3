//#define _CRTDBG_MAP_ALLOC
//#include <stdlib.h>
//#include <crtdbg.h>
#if defined(USE_OPENMP) && defined(USE_MPI)
#error "Cannot define both USE_OPENMP and USE_MPI at the same time."
#endif
#ifdef USE_MPI
#define CLOCK() MPI_Wtime()
#elif defined(USE_OPENMP)
#define CLOCK() omp_get_wtime()
#else
#define CLOCK() clock()/CLOCKS_PER_SEC
#endif
#include "PhreeqcRM.h"
#include "RMVARS.h"
#include "PHRQ_base.h"
#include "PHRQ_io.h"
#include "IPhreeqc.h"
#include "IPhreeqc.hpp"
#include "IPhreeqcPhast.h"
#include "IPhreeqcPhastLib.h"
#include "Serializer.h"
#include "StorageBin.h"
#include <assert.h>
#include "System.h"
#include "BMIVariant.h"
#include "VarManager.h"
#ifdef USE_GZ
#include <zlib.h>
#else
#define gzFile FILE*
#define gzclose fclose
#define gzopen fopen
#define gzprintf fprintf
//#define ogzstream std::ofstream
#endif
#ifdef USE_YAML
#include "yaml-cpp/yaml.h"
#endif
#include "cxxMix.h"
#include "Solution.h"
#include "Exchange.h"
#include "Surface.h"
#include "PPassemblage.h"
#include "SSassemblage.h"
#include "cxxKinetics.h"
#include "GasPhase.h"
#include "Temperature.h"
#include "Reaction.h"
#include "CSelectedOutput.hxx"

#include <time.h>

#if defined(USE_OPENMP)
#include <omp.h>
#if defined(_WIN32)
#include <windows.h>
#else
#include <unistd.h>
#endif /* _WIN32 */
#endif /* USE_OPENMP */

#include "Phreeqc.h"
#include "IPhreeqcPhast.h"

#if defined(USE_MPI)
	const MP_TYPE PhreeqcRM::default_data_for_parallel_processing = MPI_COMM_WORLD;
#else
	const MP_TYPE PhreeqcRM::default_data_for_parallel_processing = -1;
#endif

// Pimpl for initialization
class PhreeqcRM::Initializer
{

public:
	Initializer() : nxyz_arg(default_nxyz), data_for_parallel_processing(default_data_for_parallel_processing), io(nullptr) {}
	Initializer(int nxyz, MP_TYPE data, PHRQ_io *pio) : nxyz_arg(nxyz), data_for_parallel_processing(data), io(pio) {}

public:
	int nxyz_arg;
	MP_TYPE data_for_parallel_processing;
	PHRQ_io *io;
};

//// static PhreeqcRM methods
/* ---------------------------------------------------------------------- */
void
PhreeqcRM::CleanupReactionModuleInstances(void)
/* ---------------------------------------------------------------------- */
{
	PhreeqcRM::DestroyAll();
}
/* ---------------------------------------------------------------------- */
int
PhreeqcRM::CreateReactionModule(int nxyz, MP_TYPE nthreads)
/* ---------------------------------------------------------------------- */
{
	//_CrtSetDbgFlag ( _CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF );
	//_crtBreakAlloc = 5144;
	try
	{
		PhreeqcRM * Reaction_module_ptr = new PhreeqcRM(nxyz, nthreads);
		if (Reaction_module_ptr)
		{
			return Reaction_module_ptr->GetIndex();
		}
	}
	catch(...)
	{
		return IRM_OUTOFMEMORY;
	}
	return IRM_OUTOFMEMORY;
}
/* ---------------------------------------------------------------------- */
IRM_RESULT
PhreeqcRM::DestroyReactionModule(int id)
/* ---------------------------------------------------------------------- */
{
	return PhreeqcRM::Destroy(id);
}
/* ---------------------------------------------------------------------- */
void
PhreeqcRM::ErrorHandler(int result, const std::string & e_string)
/* ---------------------------------------------------------------------- */
{
	if (result < 0)
	{
		this->DecodeError(result);
		this->ErrorMessage(e_string);
		throw PhreeqcRMStop();
	}
}
#ifdef USE_YAML
/* ---------------------------------------------------------------------- */
int
PhreeqcRM::GetGridCellCountYAML(const char* YAML_file)
/* ---------------------------------------------------------------------- */
{
	std::ifstream file(YAML_file);
	if (!file.is_open()) {
		std::cerr << "YAML file not found: " << YAML_file << std::endl;
		return -1;
	}
	file.close();
	YAML::Node yaml = YAML::LoadFile(YAML_file);
	for (auto it = yaml.begin(); it != yaml.end(); it++)
	{
		YAML::Node node1 = *it;
		auto it1 = node1.begin();
		std::string key = it1++->second.as<std::string>();
		if (key == "SetGridCellCount")
		{
			int nxyz = it1->second.as<int>();
			return nxyz;
		}
	}
	return 0;
}
#endif
/*
//
// end static PhreeqcRM methods
//
*/

PhreeqcRM::PhreeqcRM(int nxyz_arg, MP_TYPE data_for_parallel_processing, PHRQ_io *io/*=NULL*/, bool delay_construct/*=false*/)
	//
	// constructor
	//
: StaticIndexer( this )
, component_h2o( true )
, phreeqc_bin( nullptr )
, count_chemistry( nxyz_arg )
, worker_waiting( false )
, error_count( 0 )
, error_handler_mode( 0 )
, need_error_check( true )
, phreeqcrm_io( io )
, delete_phreeqcrm_io( false )
, mpi_worker_callback_fortran( nullptr )
, mpi_worker_callback_c( nullptr )
, mpi_worker_callback_cookie( nullptr )
, species_save_on( false )
, initializer(std::unique_ptr<PhreeqcRM::Initializer>(new PhreeqcRM::Initializer(nxyz_arg, data_for_parallel_processing, io)))
{
#ifdef USE_MPI
	phreeqcrm_comm = data_for_parallel_processing;
	if (MPI_Comm_size(phreeqcrm_comm, &this->mpi_tasks) != MPI_SUCCESS)
	{
		this->ErrorMessage("MPI communicator not defined", 1);
	}

	if (MPI_Comm_rank(phreeqcrm_comm, &this->mpi_myself) != MPI_SUCCESS)
	{
		this->ErrorMessage("MPI communicator not defined", 1);
	}
#endif
	if (!delay_construct)
	{
#ifdef USE_MPI
		if (mpi_myself == 0)
		{
			this->Construct();
			MpiWorkerBreak();
		}
		else
		{
			MpiWorker();
		}
#else
		this->Construct();
#endif
	}
}

void PhreeqcRM::Construct()
{
	int nxyz_arg                         = this->initializer->nxyz_arg;
	MP_TYPE data_for_parallel_processing = this->initializer->data_for_parallel_processing;
	//PHRQ_io* io                          = this->initializer->io;
#ifdef USE_MPI
	if (mpi_myself == 0)
	{
		if (this->mpi_myself == 0)
		{
			int method = METHOD_CONSTRUCT;
			MPI_Bcast(&method, 1, MPI_INT, 0, phreeqcrm_comm);
		}
	}
	MPI_Bcast(&nxyz_arg, 1, MPI_INT, 0, phreeqcrm_comm);
#endif
	assert(this->phreeqc_bin == nullptr);
	this->phreeqc_bin = new cxxStorageBin();
	if (this->phreeqcrm_io == nullptr)
	{
		this->phreeqcrm_io = new PHRQ_io();
		this->delete_phreeqcrm_io = true;
	}
	this->phreeqcrm_io->Set_error_ostream(&std::cerr);
	//this->phreeqcrm_var_man = new VarManager(this);
	// second argument is threads for OPENMP or COMM for MPI
#if !defined(USE_MPI)
	int thread_count = 1;
	int n = 1;
#endif
#ifdef USE_OPENMP
	thread_count = data_for_parallel_processing;
#if defined(_WIN32)
	SYSTEM_INFO sysinfo;
	GetSystemInfo( &sysinfo );

	n = (int) sysinfo.dwNumberOfProcessors;
#else
	// Linux, Solaris, Aix, Mac 10.4+
	n = sysconf( _SC_NPROCESSORS_ONLN );
#endif
#endif
	// Determine mpi_myself
#ifdef USE_MPI
	//phreeqcrm_comm = data_for_parallel_processing;
	//if (MPI_Comm_size(phreeqcrm_comm, &this->mpi_tasks) != MPI_SUCCESS)
	//{
	//	this->ErrorMessage("MPI communicator not defined", 1);
	//}

	//if (MPI_Comm_rank(phreeqcrm_comm, &this->mpi_myself) != MPI_SUCCESS)
	//{
	//	this->ErrorMessage("MPI communicator not defined", 1);
	//}
	double standard_task = this->TimeStandardTask();
	standard_task_vector.resize(this->mpi_tasks, 0.0);
	MPI_Gather(&standard_task, 1, MPI_DOUBLE, &standard_task_vector.front(), 1, MPI_DOUBLE, 0, phreeqcrm_comm);
	if (mpi_myself == 0)
	{
		double s0 = standard_task_vector[0];
		for (int i = 0; i < this->mpi_tasks; i++)
		{
			standard_task_vector[i] = s0 / standard_task_vector[i];  // faster is smaller
			//std::cerr << "Root list of standard tasks " << i << " " << standard_task_vector[i] << std::endl;
		}
	}
#else
	this->mpi_myself = 0;
	this->mpi_tasks = 1;
#endif	
	if (mpi_myself == 0)
	{
		this->nxyz = nxyz_arg;
	}
	this->component_h2o = true;
#ifdef USE_MPI
	MPI_Bcast(&this->nxyz, 1, MPI_INT, 0, phreeqcrm_comm);
	int temp_tf = this->component_h2o ? 1 : 0;
	MPI_Bcast(&temp_tf, 1, MPI_INT, 0, phreeqcrm_comm);
	this->component_h2o = (temp_tf == 0) ? false : true;
	//MPI_Bcast(&this->component_h2o, 1, MPI_LOGICAL, 0, phreeqcrm_comm);
	this->nthreads = 1;
	if (this->mpi_myself == 0)
	{
		if (this->mpi_tasks > this->nxyz)
		{
			std::ostringstream err;
			err << "Number of threads must be less than or equal to number of model cells, ";
			err << this->nxyz << "." << std::endl;
			this->ErrorHandler(IRM_FAIL, err.str());
		}
	}
#else
	this->nthreads = (thread_count > 0) ? thread_count : n;
	//this->nthreads = std::min(this->nthreads, this->nxyz);
	this->nthreads = (this->nthreads < this->nxyz) ? this->nthreads : this->nxyz;

#endif

	// last one is to calculate well pH
	for (int i = 0; i < this->nthreads + 2; i++)
	{
		this->workers.push_back(new IPhreeqcPhast);
		this->workers.back()->Set_error_ostream(phreeqcrm_io->Get_error_ostream());
		this->workers.back()->SetErrorFileOn(true);
		this->workers.back()->Set_error_on(true);
	}
	if (this->GetWorkers()[0])
	{
		//std::map<size_t, PhreeqcRM*>::value_type instance(this->GetWorkers()[0]->Get_Index(), this);
		//PhreeqcRM::Instances.insert(instance);
	}
	else
	{
		std::cerr << "Reaction module not created." << std::endl;
		exit(4);
	}
	this->file_prefix = "myrun";
	this->dump_file_name = file_prefix;
	this->dump_file_name.append(".dump");
	this->gfw_water = 18.;						// gfw of water
	this->count_chemistry = this->nxyz;
	this->partition_uz_solids = false;
	this->time = 0;							    // scalar time from transport
	this->time_step = 0;					    // scalar time step from transport
	this->time_conversion = 1.;				    // scalar conversion factor for time
	this->rebalance_by_cell = true;
	this->rebalance_fraction = 0.5;				// parameter for rebalancing process load for parallel

	// print flags
	this->print_chemistry_on.resize(3, false);  // print flag for chemistry output file
	this->selected_output_on = true;			// Create selected output

	// Units
	this->units_Solution = 1;				    // 1 mg/L, 2 mol/L, 3 kg/kgs
	this->units_PPassemblage = 0;			    // 0, mol/L cell; 1, mol/L water; 2 mol/L rock
	this->units_Exchange = 0;			        // 0, mol/L cell; 1, mol/L water; 2 mol/L rock
	this->units_Surface = 0;			        // 0, mol/L cell; 1, mol/L water; 2 mol/L rock
	this->units_GasPhase = 0;			        // 0, mol/L cell; 1, mol/L water; 2 mol/L rock
	this->units_SSassemblage = 0;			    // 0, mol/L cell; 1, mol/L water; 2 mol/L rock
	this->units_Kinetics = 0;			        // 0, mol/L cell; 1, mol/L water; 2 mol/L rock
	this->use_solution_density_volume = true;   // Use calculated solution density and volume


	// initialize arrays
	for (int i = 0; i < this->nxyz; i++)
	{
		//forward_mapping.push_back(i);
		std::vector<int> temp;
		temp.push_back(i);
		backward_mapping.push_back(temp);
		//saturation.push_back(1.0);
		//rv.push_back(1.0);
		//porosity.push_back(0.1);
		//print_chem_mask.push_back(0);
		//density.push_back(1.0);
		//pressure.push_back(1.0);
		//tempc.push_back(25.0);
		//solution_volume.push_back(1.0);
		if (mpi_myself == 0)
		{
			forward_mapping_root.push_back(i);
			print_chem_mask_root.push_back(0);
			density_root.push_back(1.0);
			viscosity_root.push_back(1.0);
			porosity_root.push_back(0.1);
			rv_root.push_back(1.0);
			pressure_root.push_back(1.0);
			saturation_root.push_back(1.0);
		}
	}

	// set work for each thread or process
	SetEndCells();
#ifdef USE_MPI
	ScatterNchem(print_chem_mask_root, print_chem_mask_worker);
	ScatterNchem(density_root, density_worker);
	ScatterNchem(porosity_root, porosity_worker);
	ScatterNchem(rv_root, rv_worker);
	ScatterNchem(saturation_root, saturation_worker);
#endif
}
PhreeqcRM::~PhreeqcRM(void)
{
	this->CloseFiles();

	for (auto worker : this->GetWorkers())
	{
		delete worker;
	}

	delete this->phreeqc_bin;
	if (delete_phreeqcrm_io)
	{
		delete this->phreeqcrm_io;
	}
}
/* ---------------------------------------------------------------------- */
void
PhreeqcRM::AddOutputVars(std::string option, std::string def)
/* ---------------------------------------------------------------------- */
{
	// no-op
}
/* ---------------------------------------------------------------------- */
IRM_RESULT
PhreeqcRM::CellInitialize(
					int ixyz,
					int n_user_new,
					const int *initial_conditions1,
					const int *initial_conditions2,
					double *fraction1,
					std::set<std::string> &error_set)
/* ---------------------------------------------------------------------- */
{
	// ixyz is nxyz numbering
	// n_user_n is nchem cells numbering
	// For MPI, initial_conditions are local to mpi_myself (end - start + 1 in size)
	// For OpenMP, initial conditions are nxyz in size

	int n_old1, n_old2;
	double f1;
#ifdef USE_MPI
	// For nonroot, initial conditions are shortened
	int ilocal = n_user_new - this->start_cell[this->mpi_myself];
	int stride = this->end_cell[this->mpi_myself] - this->start_cell[this->mpi_myself] + 1;
#else
	int stride = this->nxyz;    // stride in initial_conditions
	int ilocal = ixyz;          // location in initial_conditions
#endif

	cxxStorageBin initial_bin;

	IRM_RESULT rtn = IRM_OK;
#ifdef USE_MPI
	double cell_porosity_local = this->porosity_worker[ilocal];
	double cell_rv_local = this->rv_worker[ilocal];
	double cell_saturation_local = this->saturation_worker[ilocal];
#else
	double cell_porosity_local = this->porosity_root[ixyz];
	double cell_rv_local = this->rv_root[ixyz];
	double cell_saturation_local = this->saturation_root[ixyz];
#endif
	std::vector < double > porosity_factor;
	porosity_factor.push_back(cell_rv_local);                              // no adjustment, per liter of rv
	porosity_factor.push_back(cell_rv_local*cell_porosity_local);          // per liter of water in rv
	porosity_factor.push_back(cell_rv_local*(1.0 - cell_porosity_local));  // per liter of rock in rv

	/*
	 *   Copy solution
	 */
	n_old1 = initial_conditions1[ilocal];
	n_old2 = initial_conditions2[ilocal];
	if (n_old1 >= 0 && phreeqc_bin->Get_Solutions().find(n_old1) == phreeqc_bin->Get_Solutions().end())
	{
		std::ostringstream e_stream;
		e_stream << "Initial condition SOLUTION " << n_old1 << " not found.";
		error_set.insert(e_stream.str());
		rtn = IRM_FAIL;
	}
	if (n_old2 >= 0 && phreeqc_bin->Get_Solutions().find(n_old2) == phreeqc_bin->Get_Solutions().end())
	{
		std::ostringstream e_stream;
		e_stream << "Initial condition SOLUTION " << n_old2 << " not found.";
		error_set.insert(e_stream.str());
		rtn = IRM_FAIL;
	}
	if (rtn == IRM_OK)
	{
		f1 = fraction1[ilocal];
		if (n_old1 >= 0)
		{
			cxxMix mx;
			// Account for saturation of cell
			double current_v = phreeqc_bin->Get_Solution(n_old1)->Get_soln_vol();
			double v = f1 * cell_porosity_local * cell_saturation_local * cell_rv_local / current_v;
			mx.Add(n_old1, v);
			if (n_old2 >= 0)
			{
				current_v = phreeqc_bin->Get_Solution(n_old2)->Get_soln_vol();
				v = (1.0 - f1) * cell_porosity_local * cell_saturation_local * cell_rv_local / current_v;
				mx.Add(n_old2, v);
			}
			cxxSolution cxxsoln(phreeqc_bin->Get_Solutions(), mx, n_user_new);
			initial_bin.Set_Solution(n_user_new, &cxxsoln);
		}
	}

	/*
	 *   Copy pp_assemblage
	 */
	n_old1 = initial_conditions1[stride + ilocal];
	n_old2 = initial_conditions2[stride + ilocal];
	if (n_old1 >= 0 && phreeqc_bin->Get_PPassemblages().find(n_old1) == phreeqc_bin->Get_PPassemblages().end())
	{
		std::ostringstream e_stream;
		e_stream << "Initial condition EQUILIBRIUM_PHASES " << n_old1 << " not found.";
		error_set.insert(e_stream.str());
		rtn = IRM_FAIL;
	}
	if (n_old2 >= 0 && phreeqc_bin->Get_PPassemblages().find(n_old2) == phreeqc_bin->Get_PPassemblages().end())
	{
		std::ostringstream e_stream;
		e_stream << "Initial condition EQUILIBRIUM_PHASES " << n_old2 << " not found.";
		error_set.insert(e_stream.str());
		rtn = IRM_FAIL;
	}
	if (rtn == IRM_OK)
	{
		f1 = fraction1[stride + ilocal];
		if (n_old1 >= 0)
		{
			cxxMix mx;
			mx.Add(n_old1, f1);
			if (n_old2 >= 0)
				mx.Add(n_old2, 1 - f1);

			mx.Multiply(porosity_factor[this->units_PPassemblage]);
			cxxPPassemblage cxxentity(phreeqc_bin->Get_PPassemblages(), mx,
				n_user_new);
			initial_bin.Set_PPassemblage(n_user_new, &cxxentity);
		}
	}
	/*
	 *   Copy exchange assemblage
	 */

	n_old1 = initial_conditions1[2 * stride + ilocal];
	n_old2 = initial_conditions2[2 * stride + ilocal];
	if (n_old1 >= 0 && phreeqc_bin->Get_Exchangers().find(n_old1) == phreeqc_bin->Get_Exchangers().end())
	{
		std::ostringstream e_stream;
		e_stream << "Initial condition EXCHANGE " << n_old1 << " not found.";
		error_set.insert(e_stream.str());
		rtn = IRM_FAIL;
	}
	if (n_old2 >= 0 && phreeqc_bin->Get_Exchangers().find(n_old2) == phreeqc_bin->Get_Exchangers().end())
	{
		std::ostringstream e_stream;
		e_stream << "Initial condition EXCHANGE " << n_old2 << " not found.";
		error_set.insert(e_stream.str());
		rtn = IRM_FAIL;
	}
	if (rtn == IRM_OK)
	{
		f1 = fraction1[2 * stride + ilocal];
		if (n_old1 >= 0)
		{
			cxxMix mx;
			mx.Add(n_old1, f1);
			if (n_old2 >= 0)
				mx.Add(n_old2, 1 - f1);
			mx.Multiply(porosity_factor[this->units_Exchange]);
			cxxExchange cxxexch(phreeqc_bin->Get_Exchangers(), mx, n_user_new);
			initial_bin.Set_Exchange(n_user_new, &cxxexch);
		}
	}
	/*
	 *   Copy surface assemblage
	 */
	n_old1 = initial_conditions1[3 * stride + ilocal];
	n_old2 = initial_conditions2[3 * stride + ilocal];
	if (n_old1 >= 0 && phreeqc_bin->Get_Surfaces().find(n_old1) == phreeqc_bin->Get_Surfaces().end())
	{
		std::ostringstream e_stream;
		e_stream << "Initial condition SURFACE " << n_old1 << " not found.";
		error_set.insert(e_stream.str());
		rtn = IRM_FAIL;
	}
	if (n_old2 >= 0 && phreeqc_bin->Get_Surfaces().find(n_old2) == phreeqc_bin->Get_Surfaces().end())
	{
		std::ostringstream e_stream;
		e_stream << "Initial condition SURFACE " << n_old2 << " not found.";
		error_set.insert(e_stream.str());
		rtn = IRM_FAIL;
	}
	if (rtn == IRM_OK)
	{
		f1 = fraction1[3 * stride + ilocal];
		if (n_old1 >= 0)
		{
			cxxMix mx;
			mx.Add(n_old1, f1);
			if (n_old2 >= 0)
				mx.Add(n_old2, 1 - f1);
			mx.Multiply(porosity_factor[this->units_Surface]);
			cxxSurface cxxentity(phreeqc_bin->Get_Surfaces(), mx, n_user_new);
			initial_bin.Set_Surface(n_user_new, &cxxentity);
		}
	}
	/*
	 *   Copy gas phase
	 */
	n_old1 = initial_conditions1[4 * stride + ilocal];
	n_old2 = initial_conditions2[4 * stride + ilocal];
	if (n_old1 >= 0 && phreeqc_bin->Get_GasPhases().find(n_old1) == phreeqc_bin->Get_GasPhases().end())
	{
		std::ostringstream e_stream;
		e_stream << "Initial condition GAS_PHASE " << n_old1 << " not found.";
		error_set.insert(e_stream.str());
		rtn = IRM_FAIL;
	}
	if (n_old2 >= 0 && phreeqc_bin->Get_GasPhases().find(n_old2) == phreeqc_bin->Get_GasPhases().end())
	{
		std::ostringstream e_stream;
		e_stream << "Initial condition GAS_PHASE " << n_old2 << " not found.";
		error_set.insert(e_stream.str());
		rtn = IRM_FAIL;
	}
	if (rtn == IRM_OK)
	{
		f1 = fraction1[4 * stride + ilocal];
		if (n_old1 >= 0)
		{
			cxxMix mx;
			mx.Add(n_old1, f1);
			if (n_old2 >= 0)
				mx.Add(n_old2, 1 - f1);
			mx.Multiply(porosity_factor[this->units_GasPhase]);
			cxxGasPhase cxxentity(phreeqc_bin->Get_GasPhases(), mx, n_user_new);
			initial_bin.Set_GasPhase(n_user_new, &cxxentity);
		}
	}
	/*
	 *   Copy solid solution
	 */
	n_old1 = initial_conditions1[5 * stride + ilocal];
	n_old2 = initial_conditions2[5 * stride + ilocal];
	if (n_old1 >= 0 && phreeqc_bin->Get_SSassemblages().find(n_old1) == phreeqc_bin->Get_SSassemblages().end())
	{
		std::ostringstream e_stream;
		e_stream << "Initial condition SOLID_SOLUTIONS " << n_old1 << " not found.";
		error_set.insert(e_stream.str());
		rtn = IRM_FAIL;
	}
	if (n_old2 >= 0 && phreeqc_bin->Get_SSassemblages().find(n_old2) == phreeqc_bin->Get_SSassemblages().end())
	{
		std::ostringstream e_stream;
		e_stream << "Initial condition SOLID_SOLUTIONS " << n_old2 << " not found.";
		error_set.insert(e_stream.str());
		rtn = IRM_FAIL;
	}
	if (rtn == IRM_OK)
	{
		f1 = fraction1[5 * stride + ilocal];
		if (n_old1 >= 0)
		{
			cxxMix mx;
			mx.Add(n_old1, f1);
			if (n_old2 >= 0)
				mx.Add(n_old2, 1 - f1);
			mx.Multiply(porosity_factor[this->units_SSassemblage]);
			cxxSSassemblage cxxentity(phreeqc_bin->Get_SSassemblages(), mx,
				n_user_new);
			initial_bin.Set_SSassemblage(n_user_new, &cxxentity);
		}
	}
	/*
	 *   Copy kinetics
	 */
	n_old1 = initial_conditions1[6 * stride + ilocal];
	n_old2 = initial_conditions2[6 * stride + ilocal];
	if (n_old1 >= 0 && phreeqc_bin->Get_Kinetics().find(n_old1) == phreeqc_bin->Get_Kinetics().end())
	{
		std::ostringstream e_stream;
		e_stream << "Initial condition KINETICS " << n_old1 << " not found.";
		error_set.insert(e_stream.str());
		rtn = IRM_FAIL;
	}
	if (n_old2 >= 0 && phreeqc_bin->Get_SSassemblages().find(n_old2) == phreeqc_bin->Get_SSassemblages().end())
	{
		std::ostringstream e_stream;
		e_stream << "Initial condition KINETICS " << n_old2 << " not found.";
		error_set.insert(e_stream.str());
		rtn = IRM_FAIL;
	}
	if (rtn == IRM_OK)
	{
		f1 = fraction1[6 * stride + ilocal];
		if (n_old1 >= 0)
		{
			cxxMix mx;
			mx.Add(n_old1, f1);
			if (n_old2 >= 0)
				mx.Add(n_old2, 1 - f1);
			mx.Multiply(porosity_factor[this->units_Kinetics]);
			cxxKinetics cxxentity(phreeqc_bin->Get_Kinetics(), mx, n_user_new);
			initial_bin.Set_Kinetics(n_user_new, &cxxentity);
		}
	}
	if (rtn == IRM_OK)
	{
		this->GetWorkers()[0]->Get_PhreeqcPtr()->cxxStorageBin2phreeqc(initial_bin);
	}
	return rtn;
}
/* ---------------------------------------------------------------------- */
std::string
PhreeqcRM::Char2TrimString(const char * str, size_t l)
/* ---------------------------------------------------------------------- */
{
	std::string stdstr;
	if (str)
	{
		if (l > 0)
		{
			size_t ll = strnlen(str, l);
			std::string tstr(str, (int) l);
			stdstr = tstr.substr(0,ll);
		}
		else
		{
			stdstr = str;
		}
	}
	stdstr = trim(stdstr);
	return stdstr;
}
#ifdef USE_MPI
/* ---------------------------------------------------------------------- */
IRM_RESULT
PhreeqcRM::CheckCells()
/* ---------------------------------------------------------------------- */
{
	std::vector<int> missing;
	for(int i = this->start_cell[this->mpi_myself]; i <= this->end_cell[this->mpi_myself]; i++)
	{
		cxxSolution *soln_ptr = this->workers[0]->Get_solution(i);
		if (soln_ptr == NULL)
		{
			missing.push_back(i);
		}
	}

	std::vector<int> r_vector;
	r_vector.resize(1);
	r_vector[0] = IRM_OK;
	if (missing.size() > 0)
	{
		std::ostringstream estr;
		estr << "Solutions not defined for these cells:\n";
		for (size_t i = 0; i < missing.size(); i++)
		{
			//estr << "Chem cell "<< i << "\n";
			estr << "Chem cell "<< i << " = Grid cell(s): ";
			for (size_t j = 0; j < backward_mapping[i].size(); j++)
			{
				estr << backward_mapping[i][j] << " ";
			}
			estr << "\n";
		}

		this->workers[0]->AddError(estr.str().c_str());
		r_vector[0] = IRM_FAIL;
	}
	HandleErrorsInternal(r_vector);
	return IRM_OK;
}
#else
/* ---------------------------------------------------------------------- */
IRM_RESULT
PhreeqcRM::CheckCells()
/* ---------------------------------------------------------------------- */
{
	IRM_RESULT return_value = IRM_OK;
	std::vector<int> missing;
	for (int n = 0; n < this->nthreads; n++)
	{
		for(int i = this->start_cell[n]; i <= this->end_cell[n]; i++)
		{
			cxxSolution *soln_ptr = this->workers[n]->Get_solution(i);
			if (soln_ptr == NULL)
			{
				missing.push_back(i);
			}
		}
	}

	if (missing.size() > 0)
	{
		std::ostringstream estr;
		estr << "Solutions not defined for these cells:\n";
		for (size_t i = 0; i < missing.size(); i++)
		{
			//estr << "Chem cell "<< i << "\n";
			estr << "Chem cell "<< i << " = Grid cell(s): ";
			for (size_t j = 0; j < backward_mapping[i].size(); j++)
			{
				estr << backward_mapping[i][j] << " ";
			}
			estr << "\n";
		}
		ErrorMessage(estr.str());
		throw PhreeqcRMStop();
	}
	return return_value;
}
#endif
/* ---------------------------------------------------------------------- */
int
PhreeqcRM::CheckSelectedOutput()
/* ---------------------------------------------------------------------- */
{
	if (!this->selected_output_on) return IRM_OK;
	IRM_RESULT return_value = IRM_OK;
#ifdef USE_MPI
	if (this->mpi_tasks <= 1) return return_value;

	// check number of selected output
	{
		int nso = (int) this->workers[0]->CSelectedOutputMap.size();
		// Gather number of selected output at root
		std::vector<int> recv_buffer;
		recv_buffer.resize(this->mpi_tasks);
		MPI_Gather(&nso, 1, MPI_INT, &recv_buffer.front(), 1, MPI_INT, 0, phreeqcrm_comm);
		for (int i = 1; i < this->mpi_tasks; i++)
		{
			if (recv_buffer[i] != recv_buffer[0])
			{
				this->ErrorHandler(IRM_FAIL, "CheckSelectedOutput, MPI processes have different number of selected output definitions.");
			}
		}
	}

	// check number of columns
	{
		std::map < int, CSelectedOutput >::iterator it = this->workers[0]->CSelectedOutputMap.begin();
		for ( ; it != this->workers[0]->CSelectedOutputMap.end(); it++)
		{
			int col = (int) it->second.GetColCount();
			// Gather number of columns at root
			std::vector<int> recv_buffer;
			recv_buffer.resize(this->mpi_tasks);
			MPI_Gather(&col, 1, MPI_INT, &recv_buffer.front(), 1, MPI_INT, 0, phreeqcrm_comm);
			for (int i = 1; i < this->mpi_tasks; i++)
			{
				if (recv_buffer[i] != recv_buffer[0])
				{
					this->ErrorHandler(IRM_FAIL, "CheckSelectedOutput, MPI processes have different number of selected output columns.");
				}
			}
		}
	}

	// check headings
	{
		std::map < int, CSelectedOutput >::iterator it = this->workers[0]->CSelectedOutputMap.begin();
		for ( ; it != this->workers[0]->CSelectedOutputMap.end(); it++)
		{
			std::string headings;
			int length = 0;
			// Make string with headings
			int col = (int) it->second.GetColCount();
			for (int i = 0; i < col; i++)
			{
				CVar cvar;
				cvar = it->second.Get(0, i);
				if (cvar.type == TT_STRING)
				{
					headings.append(cvar.sVal);
				}
				else
				{
					this->ErrorHandler(IRM_FAIL, "CheckSelectedOutput, MPI processes has selected output column that is not a string.");
				}
			}


			if (this->mpi_myself == 0)
			{
				length = (int) headings.size();
			}
			MPI_Bcast(&length,  1, MPI_INT, 0, phreeqcrm_comm);

			// Broadcast string
			char *headings_bcast = new char[length + 1];
			if (this->mpi_myself == 0)
			{
				strcpy(headings_bcast, headings.c_str());
			}

			MPI_Bcast(headings_bcast, length + 1, MPI_CHAR, 0, phreeqcrm_comm);

			int equal = strcmp(headings_bcast, headings.c_str()) == 0 ? 1 : 0;
			delete [] headings_bcast;
			std::vector<int> recv_buffer;
			recv_buffer.resize(this->mpi_tasks);
			MPI_Gather(&equal, 1, MPI_INT, &recv_buffer.front(), 1, MPI_INT, 0, phreeqcrm_comm);
			if (mpi_myself == 0)
			{
				for (int i = 1; i < this->mpi_tasks; i++)
				{
					if (recv_buffer[i] == 0)
					{
						this->ErrorHandler(IRM_FAIL, "CheckSelectedOutput, MPI processes have different column headings.");
					}
				}
			}
		}
	}
	// Count rows
	{
		std::map < int, CSelectedOutput >::iterator it = this->workers[0]->CSelectedOutputMap.begin();
		for ( ; it != this->workers[0]->CSelectedOutputMap.end(); it++)
		{
			int rows = (int) it->second.GetRowCount() - 1;
			std::vector<int> recv_buffer;
			recv_buffer.resize(this->mpi_tasks);
			MPI_Gather(&rows, 1, MPI_INT, &recv_buffer.front(), 1, MPI_INT, 0, phreeqcrm_comm);
			if (this->mpi_myself == 0)
			{
				int count = 0;
				for (int n = 0; n < this->mpi_tasks; n++)
				{
					count += recv_buffer[n];
				}
				if (count != this->count_chemistry)
				{
					this->ErrorHandler(IRM_FAIL, "CheckSelectedOutput, Sum of rows is not equal to count_chem.");
				}
			}
		}
	}
#else
	if (this->nthreads <= 1) return VR_OK;

	// check number of selected output
	{
		for (int i = 1; i < this->mpi_tasks; i++)
		{
			if (this->workers[i]->CSelectedOutputMap.size() != this->workers[0]->CSelectedOutputMap.size())
			{
				this->ErrorHandler(IRM_FAIL, "CheckSelectedOutput, Threads have different number of selected output definitions.");
				return IRM_FAIL;
			}
		}
	}

	// check number of columns
	{
		for (int n = 1; n < this->mpi_tasks; n++)
		{
			std::map < int, CSelectedOutput >::iterator root_it = this->workers[0]->CSelectedOutputMap.begin();
			std::map < int, CSelectedOutput >::iterator n_it = this->workers[n]->CSelectedOutputMap.begin();
			for( ; root_it != this->workers[0]->CSelectedOutputMap.end(); root_it++, n_it++)
			{
				if (root_it->second.GetColCount() != n_it->second.GetColCount())
				{
					this->ErrorHandler(IRM_FAIL, "CheckSelectedOutput, Threads have different number of selected output columns.");
				    return IRM_FAIL;
				}
			}
		}
	}

	// check headings
	{
		for (int n = 1; n < this->mpi_tasks; n++)
		{
			std::map < int, CSelectedOutput >::iterator root_it = this->workers[0]->CSelectedOutputMap.begin();
			std::map < int, CSelectedOutput >::iterator n_it = this->workers[n]->CSelectedOutputMap.begin();
			for( ; root_it != this->workers[0]->CSelectedOutputMap.end(); root_it++, n_it++)
			{
				for (int i = 0; i < (int) root_it->second.GetColCount(); i++)
				{
					CVar root_cvar;
					root_it->second.Get(0, i, &root_cvar);
					CVar n_cvar;
					n_it->second.Get(0, i, &n_cvar);
					if (root_cvar.type != TT_STRING || n_cvar.type != TT_STRING)
					{
						this->ErrorHandler(IRM_FAIL, "CheckSelectedOutput, Threads has selected output column that is not a string.");
				        return IRM_FAIL;
					}
					if (strcmp(root_cvar.sVal, n_cvar.sVal) != 0)
					{
						this->ErrorHandler(IRM_FAIL, "CheckSelectedOutput, Threads have different column headings.");
				        return IRM_FAIL;
					}
				}
			}
		}
	}

	// Count rows
	{
		for (int nso = 0; nso < (int) this->workers[0]->CSelectedOutputMap.size(); nso++)
		{
			int n_user = this->GetNthSelectedOutputUserNumber(nso);
			int count = 0;
			for (int n = 0; n < this->nthreads; n++)
			{
				std::map < int, CSelectedOutput >::iterator n_it = this->workers[n]->CSelectedOutputMap.find(n_user);
				count += (int) n_it->second.GetRowCount() - 1;
			}
			if (count != this->count_chemistry)
			{
				this->ErrorHandler(IRM_FAIL, "CheckSelectedOutput, Sum of rows is not equal to count_chem.");
				return IRM_FAIL;
			}
		}
	}
#endif
	return return_value;
}
/* ---------------------------------------------------------------------- */
void
PhreeqcRM::ClearBMISelectedOutput()
/* ---------------------------------------------------------------------- */
{
	// no-op
}
/* ---------------------------------------------------------------------- */
IRM_RESULT
PhreeqcRM::CloseFiles(void)
/* ---------------------------------------------------------------------- */
{
	this->phreeqcrm_error_string.clear();
	// open echo and log file, prefix.log.txt
	if (phreeqcrm_io != NULL)
	{
		this->phreeqcrm_io->log_close();

		// output_file is prefix.chem.txt
		this->phreeqcrm_io->output_close();
	}

	return IRM_OK;
}
#ifdef SKIP
/* ---------------------------------------------------------------------- */
void
PhreeqcRM::Collapse2Nchem(int *i_in, int *i_out)
/* ---------------------------------------------------------------------- */
{
	if (mpi_myself == 0)
	{
		for (int j = 0; j < this->nxyz; j++)
		{
			int ichem = this->forward_mapping[j];
			if (ichem < 0) continue;
			i_out[ichem] = i_in[j];
		}
	}
}
/* ---------------------------------------------------------------------- */
void
PhreeqcRM::Collapse2Nchem(double *i_in, double *i_out)
/* ---------------------------------------------------------------------- */
{
	if (mpi_myself == 0)
	{
		for (int j = 0; j < this->nxyz; j++)
		{
			int ichem = this->forward_mapping[j];
			if (ichem < 0) continue;
			i_out[ichem] = i_in[j];
		}
	}
}
#endif
/* ---------------------------------------------------------------------- */
void
PhreeqcRM::Concentrations2Solutions(int n, std::vector<double> &c)
/* ---------------------------------------------------------------------- */
{
	this->phreeqcrm_error_string.clear();
	if (this->component_h2o)
	{
		return Concentrations2SolutionsH2O(n, c);
	}
	return Concentrations2SolutionsNoH2O(n, c);
}

/* ---------------------------------------------------------------------- */
void
PhreeqcRM::Concentrations2SolutionsH2O(int n, std::vector<double> &c)
/* ---------------------------------------------------------------------- */
{
	// assumes H2O, total H, total O, and charge are transported
	int j, k;

#ifdef USE_MPI
	int start = this->start_cell[this->mpi_myself];
	int end = this->end_cell[this->mpi_myself];
#else
	int start = this->start_cell[n];
	int end = this->end_cell[n];
#endif
	if (gfw.size() == 0)
	{
		this->ErrorMessage("FindComponents must be called before this point, stopping.", true);
		std::cerr << "ERROR: FindComponents must be called before this point, stopping." << std::endl;
		throw PhreeqcRMStop();
	}
	for (j = start; j <= end; j++)
	{
		std::vector<double> d;  // scratch space to convert from mass fraction to moles
		// j is count_chem number
#ifdef USE_MPI
		double dens = density_worker[j - start];
		double por  = porosity_worker[j - start];
		double repv  = rv_worker[j - start];
		double sat = saturation_worker[j - start];
#else
		int i = this->backward_mapping[j][0];
		double dens = density_root[i];
		double por  = porosity_root[i];
		double repv = rv_root[i];
		double sat  = saturation_root[i];
#endif
		if (sat <= 0.0) continue;
		switch (this->units_Solution)
		{
		case 1:  // mg/L to mol/L
			{
				// convert to mol/L
				for (k = 1; k < (int) this->components.size(); k++)
				{
					d.push_back(c[j * (int) this->components.size() + k] * 1e-3 / this->gfw[k]);
				}
				double h2o_mol = c[j * (int) this->components.size()] * 1e-3 / this->gfw[0];
				d[0] += h2o_mol * 2.0;
				d[1] += h2o_mol;
			}
			break;
		case 2:  // mol/L
			{
				// convert to mol/L
				for (k = 1; k < (int) this->components.size(); k++)
				{
					d.push_back(c[j * (int) this->components.size() + k]);
				}
				double h2o_mol = c[j * (int) this->components.size()];
				d[0] += h2o_mol * 2.0;
				d[1] += h2o_mol;
			}
			break;
		case 3:  // mass fraction, kg/kg solution to mol/L
			{
				// convert to mol/L
				for (k = 1; k < (int) this->components.size(); k++)
				{
					d.push_back(c[j * (int) this->components.size() + k] * 1000.0 / this->gfw[k] * dens);
				}
				double h2o_mol = c[j * (int) this->components.size()] * 1000.0 / this->gfw[0] * dens;
				d[0] += h2o_mol * 2.0;
				d[1] += h2o_mol;
			}
			break;
		}
		// convert mol/L to moles per cell
		for (k = 0; k < (int) this->components.size() - 1; k++)
		{
			if (sat > 0.0)
			{
				d[k] *= por * sat * repv;
			}
			else
			{
				d[k] *= por * repv;
			}

		}
		// update solution
		cxxNameDouble nd;
		for (k = 4; k < (int) components.size(); k++)
		{
			if (d[k-1] < 0.0) d[k-1] = 0.0;
			nd.add(components[k].c_str(), d[k-1]);
		}

		cxxSolution *soln_ptr = this->GetWorkers()[n]->Get_solution(j);
		if (soln_ptr)
		{
			soln_ptr->Update(d[0], d[1], d[2], nd);
		}
	}
	return;
}

/* ---------------------------------------------------------------------- */
void
PhreeqcRM::Concentrations2SolutionsNoH2O(int n, std::vector<double> &c)
/* ---------------------------------------------------------------------- */
{
	// assumes total H, total O, and charge are transported
	int j, k;

#ifdef USE_MPI
	int start = this->start_cell[this->mpi_myself];
	int end = this->end_cell[this->mpi_myself];
#else
	int start = this->start_cell[n];
	int end = this->end_cell[n];
#endif
	if (gfw.size() == 0)
	{
		this->ErrorMessage("FindComponents must be called before this point, stopping.", true);
		std::cerr << "ERROR: FindComponents must be called before this point, stopping." << std::endl;
		throw PhreeqcRMStop();
	}
	for (j = start; j <= end; j++)
	{
		std::vector<double> d;  // scratch space to convert from mass fraction to moles
		// j is count_chem number

#ifdef USE_MPI
		double dens = density_worker[j - start];
		double por  = porosity_worker[j - start];
		double repv = rv_worker[j - start];
		double sat  = saturation_worker[j - start];
#else
		int i = this->backward_mapping[j][0];
		double dens = density_root[i];
		double por  = porosity_root[i];
		double repv = rv_root[i];
		double sat  = saturation_root[i];
#endif
		if (sat <= 0.0) continue;
		switch (this->units_Solution)
		{
		case 1:  // mg/L to mol/L
			{
				// convert to mol/L
				for (k = 0; k < (int) this->components.size(); k++)
				{
					d.push_back(c[j * (int) this->components.size() + k] * 1e-3 / this->gfw[k]);
				}
			}
			break;
		case 2:  // mol/L
			{
				// convert to mol/L
				for (k = 0; k < (int) this->components.size(); k++)
				{
					d.push_back(c[j * (int) this->components.size() + k]);
				}
			}
			break;
		case 3:  // mass fraction, kg/kg solution to mol/L
			{
				// convert to mol/L
				for (k = 0; k < (int) this->components.size(); k++)
				{
					d.push_back(c[j * (int) this->components.size() + k] * 1000.0 / this->gfw[k] * dens);
				}
			}
			break;
		}
		// convert mol/L to moles per cell
		for (k = 0; k < (int) this->components.size(); k++)
		{
			if (sat > 0.0)
			{
				d[k] *= por * sat * repv;
			}
			else
			{
				d[k] *= por * repv;
			}
		}
		// update solution
		cxxNameDouble nd;
		for (k = 3; k < (int) components.size(); k++)
		{
			if (d[k] < 0.0) d[k] = 0.0;
			nd.add(components[k].c_str(), d[k]);
		}

		cxxSolution *soln_ptr = this->GetWorkers()[n]->Get_solution(j);
		if (soln_ptr)
		{
			soln_ptr->Update(d[0], d[1], d[2], nd);
		}
	}
	return;
}
/* ---------------------------------------------------------------------- */
IPhreeqc *
PhreeqcRM::Concentrations2Utility(const std::vector<double> &c, 
	const std::vector<double> &tc, const std::vector<double> &p_atm)
/* ---------------------------------------------------------------------- */
{
	this->phreeqcrm_error_string.clear();
	if (this->component_h2o)
	{
		return Concentrations2UtilityH2O(c, tc, p_atm);
	}
	return Concentrations2UtilityNoH2O(c, tc, p_atm);
}
/* ---------------------------------------------------------------------- */
IPhreeqc *
PhreeqcRM::Concentrations2UtilityH2O(const std::vector<double> &c_in, 
	const std::vector<double> &tc, const std::vector<double> &p_atm)
/* ---------------------------------------------------------------------- */
{
	std::vector<double> c = c_in;
	size_t ncomps = this->components.size();
	size_t nsolns = c.size() / ncomps;
	size_t nutil= this->nthreads + 1;
	if (gfw.size() == 0)
	{
		this->ErrorMessage("FindComponents must be called before this point, stopping.", true);
		std::cerr << "ERROR: FindComponents must be called before this point, stopping." << std::endl;
		throw PhreeqcRMStop();
	}
	for (size_t i = 0; i < nsolns; i++)
	{
		std::vector<double> d;  // scratch space to convert from mass fraction to moles
		switch (this->units_Solution)
		{
		case 1:  // mg/L to mol/L
			{
				double *ptr = &c[i];
				// convert to mol/L
				for (size_t k = 1; k < this->components.size(); k++)
				{
					d.push_back(ptr[nsolns * k] * 1e-3 / this->gfw[k]);
				}
				double h2o_mol = ptr[0] * 1e-3 / this->gfw[0];
				d[0] += h2o_mol * 2.0;
				d[1] += h2o_mol;
			}
			break;
		case 2:  // mol/L
			{
				double *ptr = &c[i];
				// convert to mol/L
				for (size_t k = 1; k < this->components.size(); k++)
				{
					d.push_back(ptr[nsolns * k]);
				}
				double h2o_mol = ptr[0];
				d[0] += h2o_mol * 2.0;
				d[1] += h2o_mol;
			}
			break;
		case 3:  // mass fraction, kg/kg solution to mol/kgs
			{
				double *ptr = &c[i];
				// convert to mol/L
				for (size_t k = 1; k < this->components.size(); k++)
				{
					d.push_back(ptr[nsolns * k] * 1000.0 / this->gfw[k]);
				}
				double h2o_mol = ptr[0] * 1000.0 / this->gfw[0] * density_root[i];
				d[0] += h2o_mol * 2.0;
				d[1] += h2o_mol;
			}
			break;
		}

		// update solution
		cxxNameDouble nd;
		for (size_t k = 4; k < components.size(); k++)
		{
			if (d[k-1] < 0.0) d[k-1] = 0.0;
			nd.add(components[k].c_str(), d[k-1]);
		}
		cxxSolution soln;
		if (d[0] > 0.0 && d[1] > 0.0)
		{
			soln.Update(d[0], d[1], d[2], nd);
		}
		soln.Set_tc(tc[i]);
		soln.Set_patm(p_atm[i]);
		this->workers[nutil]->PhreeqcPtr->Rxn_solution_map[(int) (i + 1)] = soln;
	}
	return (dynamic_cast< IPhreeqc *> (this->workers[nutil]));
}

/* ---------------------------------------------------------------------- */
IPhreeqc *
PhreeqcRM::Concentrations2UtilityNoH2O(const std::vector<double> &c_in,
	const std::vector<double> &tc, const std::vector<double> &p_atm)
/* ---------------------------------------------------------------------- */
{
	std::vector<double> c = c_in;
	size_t ncomps = this->components.size();
	size_t nsolns = c.size() / ncomps;
	size_t nutil= this->nthreads + 1;
	if (gfw.size() == 0)
	{
		this->ErrorMessage("FindComponents must be called before this point, stopping.", true);
		std::cerr << "ERROR: FindComponents must be called before this point, stopping." << std::endl;
		throw PhreeqcRMStop();
	}
	for (size_t i = 0; i < nsolns; i++)
	{
		std::vector<double> d;  // scratch space to convert from mass fraction to moles
		switch (this->units_Solution)
		{
		case 1:  // mg/L to mol/L
			{
				double *ptr = &c[i];
				// convert to mol/L
				for (size_t k = 0; k < this->components.size(); k++)
				{
					d.push_back(ptr[nsolns * k] * 1e-3 / this->gfw[k]);
				}
			}
			break;
		case 2:  // mol/L
			{
				double *ptr = &c[i];
				// convert to mol/L
				for (size_t k = 0; k < this->components.size(); k++)
				{
					d.push_back(ptr[nsolns * k]);
				}
			}
			break;
		case 3:  // mass fraction, kg/kg solution to mol/kgs
			{
				double *ptr = &c[i];
				// convert to mol/L
				for (size_t k = 0; k < this->components.size(); k++)
				{
					d.push_back(ptr[nsolns * k] * 1000.0 / this->gfw[k]);
				}
			}
			break;
		}

		// update solution
		cxxNameDouble nd;
		for (size_t k = 3; k < components.size(); k++)
		{
			if (d[k] < 0.0) d[k] = 0.0;
			nd.add(components[k].c_str(), d[k]);
		}
		cxxSolution soln;
		if (d[0] > 0.0 && d[1] > 0.0)
		{
			soln.Update(d[0], d[1], d[2], nd);
		}
		soln.Set_tc(tc[i]);
		soln.Set_patm(p_atm[i]);
		this->workers[nutil]->PhreeqcPtr->Rxn_solution_map[(int) (i + 1)] = soln;
	}
	return (dynamic_cast< IPhreeqc *> (this->workers[nutil]));
}
#ifdef SKIP
/* ---------------------------------------------------------------------- */
IRM_RESULT
PhreeqcRM::CreateMapping(std::vector<int> &grid2chem)
/* ---------------------------------------------------------------------- */
{
	this->phreeqcrm_error_string.clear();
	IRM_RESULT return_value = IRM_OK;
	try
	{
		if (mpi_myself == 0)
		{
			if ((int) grid2chem.size() != this->nxyz)
			{
				this->ErrorHandler(IRM_INVALIDARG, "Mapping vector is the wrong size");
			}
		}
#ifdef USE_MPI
		if (mpi_myself == 0)
		{
			int method = METHOD_CREATEMAPPING;
			MPI_Bcast(&method, 1, MPI_INT, 0, phreeqcrm_comm);
		}
		else
		{
			grid2chem.resize(this->nxyz);
		}
		MPI_Bcast(&grid2chem.front(), this->nxyz, MPI_INT, 0, phreeqcrm_comm);
#endif
		backward_mapping.clear();
		forward_mapping.clear();

		// find count_chem
		this->count_chemistry = 0;
		for (int i = 0; i < this->nxyz; i++)
		{
			if (grid2chem[i] > count_chemistry)
			{
				count_chemistry = grid2chem[i];
			}
		}
		count_chemistry ++;

#ifdef USE_MPI
		if (this->mpi_myself == 0)
		{
			if (this->mpi_tasks > this->count_chemistry)
			{
				std::ostringstream err;
				err << "Number of threads must be less than or equal to number of reaction cells, ";
				err << this->count_chemistry << "." << std::endl;
				this->ErrorHandler(IRM_FAIL, err.str());
			}
		}
#else
		if (this->nthreads > this->count_chemistry)
		{
			std::ostringstream err;
			err << "Number of threads must be less than or equal to number of reaction cells, ";
			err << this->count_chemistry << "." << std::endl;
			this->ErrorHandler(IRM_FAIL, err.str());
		}
#endif


		for (int i = 0; i < count_chemistry; i++)
		{
			std::vector<int> temp;
			backward_mapping.push_back(temp);
		}
		for (int i = 0; i < this->nxyz; i++)
		{
			int n = grid2chem[i];
			if (n >= count_chemistry)
			{
				this->ErrorHandler(IRM_INVALIDARG, "PhreeqcRM::CreateMapping, cell out of range in mapping (grid to chem).");
			}

			// copy to forward
			forward_mapping.push_back(n);

			// add to back
			if (n >= 0)
			{
				backward_mapping[n].push_back(i);
			}
		}

		// set -1 for back items > 0
		for (int i = 0; i < this->count_chemistry; i++)
		{
			// add to back
			for (size_t j = 1; j < backward_mapping[i].size(); j++)
			{
				int n = backward_mapping[i][j];
				forward_mapping[n] = -1;
			}
		}
		// check that all count_chem have at least 1 cell
		for (int i = 0; i < this->count_chemistry; i++)
		{
			if (backward_mapping[i].size() == 0)
			{
				this->ErrorHandler(IRM_INVALIDARG, "PhreeqcRM::CreateMapping, building inverse mapping (chem to grid).");
			}
		}

		// Distribute work with new count_chemistry
		SetEndCells();
	}
	catch (...)
	{
		return_value = IRM_FAIL;
	}
	return this->ReturnHandler(return_value, "PhreeqcRM::CreateMapping");
}
#endif
#ifdef SKIP
/* ---------------------------------------------------------------------- */
IRM_RESULT
PhreeqcRM::CreateMapping(std::vector<int> &grid2chem)
/* ---------------------------------------------------------------------- */
{
	this->phreeqcrm_error_string.clear();
	IRM_RESULT return_value = IRM_OK;
	try
	{
		if (mpi_myself == 0)
		{
			if ((int) grid2chem.size() != this->nxyz)
			{
				this->ErrorHandler(IRM_INVALIDARG, "Mapping vector is the wrong size");
			}
#ifdef USE_MPI
			int method = METHOD_CREATEMAPPING;
			MPI_Bcast(&method, 1, MPI_INT, 0, phreeqcrm_comm);
#endif

			backward_mapping_root.clear();
			forward_mapping_root.clear();

			// find count_chem
			this->count_chemistry = 0;
			for (int i = 0; i < this->nxyz; i++)
			{
				if (grid2chem[i] > count_chemistry)
				{
					count_chemistry = grid2chem[i];
				}
			}
			count_chemistry ++;

#ifdef USE_MPI
			if (this->mpi_tasks > this->count_chemistry)
			{
				std::ostringstream err;
				err << "Number of threads must be less than or equal to number of reaction cells, ";
				err << this->count_chemistry << "." << std::endl;
				this->ErrorHandler(IRM_FAIL, err.str());
			}
#else
			if (this->nthreads > this->count_chemistry)
			{
				std::ostringstream err;
				err << "Number of threads must be less than or equal to number of reaction cells, ";
				err << this->count_chemistry << "." << std::endl;
				this->ErrorHandler(IRM_FAIL, err.str());
			}
#endif
		}

#ifdef USE_MPI
		MPI_Bcast(&count_chemistry, 1, MPI_INT, 0, phreeqcrm_comm);
#endif
		if (this->mpi_myself == 0)
		{
			for (int i = 0; i < count_chemistry; i++)
			{
				std::vector<int> temp;
				backward_mapping_root.push_back(temp);
			}
			for (int i = 0; i < this->nxyz; i++)
			{
				int n = grid2chem[i];
				if (n >= count_chemistry)
				{
					this->ErrorHandler(IRM_INVALIDARG, "PhreeqcRM::CreateMapping, cell out of range in mapping (grid to chem).");
				}

				// copy to forward
				forward_mapping_root.push_back(n);

				// add to back
				if (n >= 0)
				{
					backward_mapping_root[n].push_back(i);
				}
			}

			// set -1 for back items > 0
			for (int i = 0; i < this->count_chemistry; i++)
			{
				// add to back
				for (size_t j = 1; j < backward_mapping_root[i].size(); j++)
				{
					int n = backward_mapping_root[i][j];
					forward_mapping_root[n] = -1;
				}
			}
			// check that all count_chem have at least 1 cell
			for (int i = 0; i < this->count_chemistry; i++)
			{
				if (backward_mapping_root[i].size() == 0)
				{
					this->ErrorHandler(IRM_INVALIDARG, "PhreeqcRM::CreateMapping, building inverse mapping (chem to grid).");
				}
			}
		}
		// Distribute work with new count_chemistry
		SetEndCells();
	}
	catch (...)
	{
		return_value = IRM_FAIL;
	}
	return this->ReturnHandler(return_value, "PhreeqcRM::CreateMapping");
}
#endif
/* ---------------------------------------------------------------------- */
IRM_RESULT
PhreeqcRM::CreateMapping(const std::vector<int> &grid2chem_in)
/* ---------------------------------------------------------------------- */
{
	this->phreeqcrm_error_string.clear();
	IRM_RESULT return_value = IRM_OK;
	try
	{
		std::vector<int> grid2chem = grid2chem_in;
		if (mpi_myself == 0)
		{
			if ((int) grid2chem.size() != this->nxyz)
			{
				this->ErrorHandler(IRM_INVALIDARG, "Mapping vector is the wrong size");
			}
		}
#ifdef USE_MPI
		if (mpi_myself == 0)
		{
			int method = METHOD_CREATEMAPPING;
			MPI_Bcast(&method, 1, MPI_INT, 0, phreeqcrm_comm);
		}
		else
		{
			grid2chem.resize(this->nxyz);
		}
		MPI_Bcast(&grid2chem.front(), this->nxyz, MPI_INT, 0, phreeqcrm_comm);
#endif
		backward_mapping.clear();
		forward_mapping_root.clear();

		// find count_chem
		this->count_chemistry = 0;
		for (int i = 0; i < this->nxyz; i++)
		{
			if (grid2chem[i] > count_chemistry)
			{
				count_chemistry = grid2chem[i];
			}
		}
		count_chemistry ++;

#ifdef USE_MPI
		if (this->mpi_myself == 0)
		{
			if (this->mpi_tasks > this->count_chemistry)
			{
				std::ostringstream err;
				err << "Number of threads must be less than or equal to number of reaction cells, ";
				err << this->count_chemistry << "." << std::endl;
				this->ErrorHandler(IRM_FAIL, err.str());
			}
		}
#else
		if (this->nthreads > this->count_chemistry)
		{
			std::ostringstream err;
			err << "Number of threads must be less than or equal to number of reaction cells, ";
			err << this->count_chemistry << "." << std::endl;
			this->ErrorHandler(IRM_FAIL, err.str());
		}
#endif


		for (int i = 0; i < count_chemistry; i++)
		{
			std::vector<int> temp;
			backward_mapping.push_back(temp);
		}
		for (int i = 0; i < this->nxyz; i++)
		{
			int n = grid2chem[i];
			if (n >= count_chemistry)
			{
				this->ErrorHandler(IRM_INVALIDARG, "PhreeqcRM::CreateMapping, cell out of range in mapping (grid to chem).");
			}

			// copy to forward
			if (mpi_myself == 0)
			{
				forward_mapping_root.push_back(n);
			}

			// add to back
			if (n >= 0)
			{
				backward_mapping[n].push_back(i);
			}
		}

		// set -1 for back items > 0
		for (int i = 0; i < this->count_chemistry; i++)
		{
			// add to back
			if (mpi_myself == 0)
			{
				for (size_t j = 1; j < backward_mapping[i].size(); j++)
				{
					int n = backward_mapping[i][j];
					forward_mapping_root[n] = -1;
				}
			}
		}
		// check that all count_chem have at least 1 cell
		for (int i = 0; i < this->count_chemistry; i++)
		{
			if (backward_mapping[i].size() == 0)
			{
				std::ostringstream estrm;
				estrm << "Largest number in grid2chem is " << count_chemistry << "." << std::endl;
				estrm << "grid2chem should contain all i such that 0 <= i < " << count_chemistry << "." << std::endl;
				estrm << "However, grid2chem does not contain the integer " << i << "." << std::endl;
				this->ErrorMessage(estrm.str().c_str());
				this->ErrorHandler(IRM_INVALIDARG, "PhreeqcRM::CreateMapping, building inverse mapping (chem to grid).");
			}
		}

		// Distribute work with new count_chemistry
		SetEndCells();
	}
	catch (...)
	{
		return_value = IRM_FAIL;
	}
	return this->ReturnHandler(return_value, "PhreeqcRM::CreateMapping");
}
/* ---------------------------------------------------------------------- */
void
PhreeqcRM::cxxSolution2concentration(cxxSolution * cxxsoln_ptr, std::vector<double> & d, double v, double dens)
/* ---------------------------------------------------------------------- */
{
	if (this->component_h2o)
	{
		return cxxSolution2concentrationH2O(cxxsoln_ptr, d, v, dens);
	}
	return cxxSolution2concentrationNoH2O(cxxsoln_ptr, d, v, dens);
}
/* ---------------------------------------------------------------------- */
void
PhreeqcRM::cxxSolution2concentrationH2O(cxxSolution * cxxsoln_ptr, std::vector<double> & d, double v, double dens)
/* ---------------------------------------------------------------------- */
{
	d.clear();
	if (gfw.size() == 0)
	{
		this->ErrorMessage("FindComponents must be called before this point, stopping.", true);
		std::cerr << "ERROR: FindComponents must be called before this point, stopping." << std::endl;
		throw PhreeqcRMStop();
	}
	// Simplify totals
	{
	  cxxNameDouble nd = cxxsoln_ptr->Get_totals().Simplify_redox();
	  cxxsoln_ptr->Set_totals(nd);
	}

	// convert units
	switch (this->units_Solution)
	{
	case 1:  // convert to mg/L
		{
			d.push_back(cxxsoln_ptr->Get_mass_water() * 1.0e6 / v);
			double moles_h2o = cxxsoln_ptr->Get_mass_water() * 1.0e3 / this->gfw[0];
			double excess_h = cxxsoln_ptr->Get_total_h() - 2.0 * moles_h2o;
			double excess_o = cxxsoln_ptr->Get_total_o() - moles_h2o;
			d.push_back(excess_h * this->gfw[1] * 1000. / v);
			d.push_back(excess_o * this->gfw[2] * 1000. / v);
			d.push_back(cxxsoln_ptr->Get_cb() * this->gfw[3] * 1000. / v);
			for (size_t i = 4; i < this->components.size(); i++)
			{
				d.push_back(cxxsoln_ptr->Get_total(components[i].c_str()) * this->gfw[i] * 1000. / v);
			}
		}
		break;
	case 2:  // convert to mol/L
		{
			double moles_h2o = cxxsoln_ptr->Get_mass_water() * 1.0e3 / this->gfw[0];
			d.push_back(moles_h2o / v);
			double excess_h = cxxsoln_ptr->Get_total_h() - 2.0 * moles_h2o;
			double excess_o = cxxsoln_ptr->Get_total_o() - moles_h2o;
			d.push_back(excess_h / v);
			d.push_back(excess_o / v);
			d.push_back(cxxsoln_ptr->Get_cb()  / v);
			for (size_t i = 4; i < this->components.size(); i++)
			{
				d.push_back(cxxsoln_ptr->Get_total(components[i].c_str())  / v);
			}
		}
		break;
	case 3:  // convert to mass fraction kg/kgs
		{
			double kgs = dens * v;
			double moles_h2o = cxxsoln_ptr->Get_mass_water() * 1.0e3 / this->gfw[0];
			d.push_back(cxxsoln_ptr->Get_mass_water() / kgs);
			double excess_h = cxxsoln_ptr->Get_total_h() - 2.0 * moles_h2o;
			double excess_o = cxxsoln_ptr->Get_total_o() - moles_h2o;
			d.push_back(excess_h * this->gfw[1] / 1000. / kgs);
			d.push_back(excess_o * this->gfw[2] / 1000. / kgs);
			d.push_back(cxxsoln_ptr->Get_cb() * this->gfw[3] / 1000. / kgs);
			for (size_t i = 4; i < this->components.size(); i++)
			{
				d.push_back(cxxsoln_ptr->Get_total(components[i].c_str()) * this->gfw[i] / 1000. / kgs);
			}
		}
		break;
	}
}

/* ---------------------------------------------------------------------- */
void
PhreeqcRM::cxxSolution2concentrationNoH2O(cxxSolution * cxxsoln_ptr, std::vector<double> & d, double v, double dens)
/* ---------------------------------------------------------------------- */
{
	d.clear();

	// Simplify totals
	{
	  cxxNameDouble nd = cxxsoln_ptr->Get_totals().Simplify_redox();
	  cxxsoln_ptr->Set_totals(nd);
	}
	if (gfw.size() == 0)
	{
		this->ErrorMessage("FindComponents must be called before this point, stopping.", true);
		std::cerr << "ERROR: FindComponents must be called before this point, stopping." << std::endl;
		throw PhreeqcRMStop();
	}
	// convert units
	switch (this->units_Solution)
	{
	case 1:  // convert to mg/L
		{
			d.push_back(cxxsoln_ptr->Get_total_h() * this->gfw[0] * 1000. / v);
			d.push_back(cxxsoln_ptr->Get_total_o() * this->gfw[1] * 1000. / v);
			d.push_back(cxxsoln_ptr->Get_cb() * this->gfw[2] * 1000. / v);
			for (size_t i = 3; i < this->components.size(); i++)
			{
				d.push_back(cxxsoln_ptr->Get_total(components[i].c_str()) * this->gfw[i] * 1000. / v);
			}
		}
		break;
	case 2:  // convert to mol/L
		{
			d.push_back(cxxsoln_ptr->Get_total_h() / v);
			d.push_back(cxxsoln_ptr->Get_total_o() / v);
			d.push_back(cxxsoln_ptr->Get_cb()  / v);
			for (size_t i = 3; i < this->components.size(); i++)
			{
				d.push_back(cxxsoln_ptr->Get_total(components[i].c_str())  / v);
			}
		}
		break;
	case 3:  // convert to mass fraction kg/kgs
		{
			double kgs = dens * v;
			d.push_back(cxxsoln_ptr->Get_total_h() * this->gfw[0] / 1000. / kgs);
			d.push_back(cxxsoln_ptr->Get_total_o() * this->gfw[1] / 1000. / kgs);
			d.push_back(cxxsoln_ptr->Get_cb() * this->gfw[2] / 1000. / kgs);
			for (size_t i = 3; i < this->components.size(); i++)
			{
				d.push_back(cxxsoln_ptr->Get_total(components[i].c_str()) * this->gfw[i] / 1000. / kgs);
			}
		}
		break;
	}
}

/* ---------------------------------------------------------------------- */
void
PhreeqcRM::DecodeError(int r)
/* ---------------------------------------------------------------------- */
{
	if (r < 0)
	{
		switch (r)
		{
		case IRM_OK:
			break;
		case IRM_OUTOFMEMORY:
			this->ErrorMessage("Out of memory.");
			break;
		case IRM_BADVARTYPE:
			this->ErrorMessage("Bad variable type.");
			break;
		case IRM_INVALIDARG:
			this->ErrorMessage("Invalid argument.");
			break;
		case IRM_INVALIDROW:
			this->ErrorMessage("Invalid row number.");
			break;
		case IRM_INVALIDCOL:
			this->ErrorMessage("Invalid column number.");
			break;
		case IRM_BADINSTANCE:
			this->ErrorMessage("Bad PhreeqcRM id.");
			break;
		case IRM_FAIL:
			this->ErrorMessage("PhreeqcRM failed.");
			break;
		default:
			this->ErrorMessage("Unknown error code.");
			break;
		}
	}
}

//#define ORIGINALDUMP
#define NEWDUMP
#ifdef USE_MPI
#ifdef NEWDUMP
/* ---------------------------------------------------------------------- */
IRM_RESULT
PhreeqcRM::DumpModule(bool dump_on, bool append)
/* ---------------------------------------------------------------------- */
{
	this->phreeqcrm_error_string.clear();
	if (this->mpi_myself == 0)
	{
		int method = METHOD_DUMPMODULE;
		MPI_Bcast(&method, 1, MPI_INT, 0, phreeqcrm_comm);
	}
	bool dump = false;

	// return if dump_on is false
	if (this->mpi_myself == 0)
	{
		dump = dump_on;
	}
	int temp_tf = dump ? 1 : 0;
	MPI_Bcast(&temp_tf, 1, MPI_INT, 0, phreeqcrm_comm);
	dump = (temp_tf == 0) ? false : true;
	//MPI_Bcast(&dump, 1, MPI_LOGICAL, 0, phreeqcrm_comm);
	if (!dump) return IRM_OK;

	IRM_RESULT return_value = IRM_OK;

	// Open file on root
	gzFile dump_file = NULL;
	try
	{
		if (this->mpi_myself == 0)
		{
			// open dump file
			std::string name(this->dump_file_name);
			std::string mode;
#ifdef USE_GZ
			mode = append ? "ab1" : "wb1";
#else
			mode = append ? "a" : "w";
#endif
			dump_file = gzopen(name.c_str(), mode.c_str());
			if (dump_file == NULL)
			{
				std::ostringstream errstr;
				errstr << "Restart file could not be opened: " << name;
				this->ErrorHandler(IRM_FAIL, errstr.str());
			}
		}
	}
	catch (...)
	{
		return_value = IRM_FAIL;
	}

	// Return on error opening dump file
	MPI_Bcast(&return_value, 1, MPI_INT, 0, phreeqcrm_comm);
	if (return_value != IRM_OK)
	{
		return this->ReturnHandler(return_value, "PhreeqcRM::DumpModule");
	}
	int block = 5000;
	// Calculate max
	int max = 0;
	for (int n = 0; n < mpi_tasks; n++)
	{
		int count = this->end_cell[n] - this->start_cell[n] + 1;
		max = count > max ? count : max;
	}

	int nblocks = max / block;
	if (max % block > 0) nblocks += 1;


	std::vector<char> char_buffer;
	const size_t gzblock = 4094;
	char buffer[gzblock + 2];
	int total_cells = this->end_cell[this->mpi_tasks - 1];
	if (total_cells <= 0) total_cells = 1;
	int pct = 10;
	int block_count = 0;
	if (mpi_myself == 0)
	{
		//std::cerr << "Dump 0% ";
		std::ostringstream msg;
		this->ScreenMessage("Dump 0% ");
	}
	// Try for dumping data
	try
	{
		// write dump file data
		//this->workers[0]->SetDumpStringOn(true);

		for (int iblock = 0; iblock < nblocks; iblock++)
		{
			std::ostringstream oss;
			std::string dump_string;
			cxxStorageBin sz_uz;
			for (int n = 0; n < this->mpi_tasks; n++)
			{
				int begin = this->start_cell[n] + iblock * block;
				if (begin <= this->end_cell[n])
				{
					int last = this->start_cell[n] + (iblock + 1) * block - 1;
					if (last > this->end_cell[n]) last = this->end_cell[n];
					if (mpi_myself == 0)
					{
						block_count += last - begin + 1;
					}


					// Dump block of cells
					if (this->mpi_myself == n)
					{
						this->workers[0]->Get_PhreeqcPtr()->phreeqc2cxxStorageBin(sz_uz);
						if (this->partition_uz_solids)
						{
							sz_uz.Add_uz(this->workers[0]->uz_bin);
						}
						sz_uz.dump_raw_range(oss, begin, last, 2);
						dump_string = oss.str();
					}
					//if (this->mpi_myself == n)
					//{
					//	// Dump block of cells
					//	std::ostringstream in;
					//	in << "DUMP; -cells " << begin << "-" << last << "\n";
					//	int status = this->workers[0]->RunString(in.str().c_str());
					//	if (status != 0)
					//	{
					//		this->ErrorMessage(this->workers[0]->GetErrorString());
					//	}
					//	this->ErrorHandler(PhreeqcRM::Int2IrmResult(status, false), "RunString");
					//}
				}
			}
			if (mpi_myself == 0)
			{
				size_t dump_length = strlen(dump_string.c_str());
				const char * start = dump_string.c_str();
				const char * end = &(dump_string.c_str()[dump_length]);
				for (const char * ptr = start; ptr < end; ptr += gzblock)
				{
					strncpy(buffer, ptr, gzblock);
					buffer[gzblock] = '\0';
					int err = gzprintf(dump_file, "%s", buffer);
					if (err <= 0)
					{
						this->ErrorHandler(IRM_FAIL, "gzprintf");
					}
				}
				//size_t dump_length = strlen(this->GetWorkers()[0]->GetDumpString());
				//const char * start = this->GetWorkers()[0]->GetDumpString();
				//const char * end = &this->GetWorkers()[0]->GetDumpString()[dump_length];
				//for (const char * ptr = start; ptr < end; ptr += gzblock)
				//{
				//	strncpy(buffer, ptr, gzblock);
				//	buffer[gzblock] = '\0';
				//	int err = gzprintf(dump_file, "%s", buffer);
				//	if (err <= 0)
				//	{
				//		this->ErrorHandler(IRM_FAIL, "gzprintf");
				//	}
				//}
			}
			for (int n = 1; n < this->mpi_tasks; n++)
			{
				// Need to transfer output stream to root and print

				if (mpi_myself == n)
				{
					int size = (int)strlen(dump_string.c_str());
					MPI_Send(&size, 1, MPI_INT, 0, 0, phreeqcrm_comm);
					MPI_Send((void *) dump_string.c_str(), size, MPI_CHAR, 0, 0, phreeqcrm_comm);
					//int size = (int)strlen(this->workers[0]->GetDumpString());
					//MPI_Send(&size, 1, MPI_INT, 0, 0, phreeqcrm_comm);
					//MPI_Send((void *) this->workers[0]->GetDumpString(), size, MPI_CHAR, 0, 0, phreeqcrm_comm);
				}
				else if (this->mpi_myself == 0)
				{
					MPI_Status mpi_status;
					int size;
					MPI_Recv(&size, 1, MPI_INT, n, 0, phreeqcrm_comm, &mpi_status);
					char_buffer.resize(size + 1);
					MPI_Recv((void *)&char_buffer.front(), size, MPI_CHAR, n, 0, phreeqcrm_comm, &mpi_status);
					char_buffer[size] = '\0';

					char * start = &char_buffer.front();
					char * end = &char_buffer[size];
					for (const char * ptr = start; ptr < end; ptr += gzblock)
					{
						strncpy(buffer, ptr, gzblock);
						buffer[gzblock] = '\0';
						int err = gzprintf(dump_file, "%s", buffer);
						if (err <= 0)
						{
							this->ErrorHandler(IRM_FAIL, "gzprintf");
						}
					}
				}
			}
			if (mpi_myself == 0 && block_count * 100 / total_cells > pct)
			{
				int pct_block_count = (block_count * 10 / total_cells) * 10;
				if (pct_block_count < 100)
				{
					//std::cerr << pct_block_count << "% ";
					std::ostringstream msg;
					msg << pct_block_count << "% ";
					this->ScreenMessage(msg.str().c_str());
				}
				pct = pct_block_count + 10;
			}
			MPI_Barrier(phreeqcrm_comm);
		}
		if (mpi_myself == 0)
		{
			std::cerr << "100% " << std::endl;
		}

		// Clear dump string to save space
		std::ostringstream clr;
		clr << "END\n";
		{
			int status;
			status = this->GetWorkers()[0]->RunString(clr.str().c_str());
			if (status != 0)
			{
				this->ErrorMessage(this->workers[0]->GetErrorString());
			}
		}
	}
	catch (...)
	{
		return_value = IRM_FAIL;
	}
	if (mpi_myself == 0)
	{
		gzclose(dump_file);
	}

	return this->ReturnHandler(return_value, "PhreeqcRM::DumpModule");
}
#endif
#ifdef ORIGINADUMP
/* ---------------------------------------------------------------------- */
IRM_RESULT
PhreeqcRM::DumpModule(bool dump_on, bool append)
/* ---------------------------------------------------------------------- */
{
	this->phreeqcrm_error_string.clear();
	if (this->mpi_myself == 0)
	{
		int method = METHOD_DUMPMODULE;
		MPI_Bcast(&method, 1, MPI_INT, 0, phreeqcrm_comm);
	}
	bool dump = false;

	// return if dump_on is false
	if (this->mpi_myself == 0)
	{
		dump = dump_on;
	}
	int temp_tf = dump ? 1 : 0;
	MPI_Bcast(&temp_tf, 1, MPI_INT, 0, phreeqcrm_comm);
	dump = (temp_tf == 0) ? false : true;
	//MPI_Bcast(&dump, 1, MPI_LOGICAL, 0, phreeqcrm_comm);
	if (!dump) return IRM_OK;

	IRM_RESULT return_value = IRM_OK;

	// Open file on root
	gzFile dump_file = NULL;
	try
	{
		if (this->mpi_myself == 0)
		{
			// open dump file
			std::string name(this->dump_file_name);
			std::string mode;
#ifdef USE_GZ
			mode = append ? "ab1" : "wb1";
#else
			mode = append ? "a" : "w";
#endif
			dump_file = gzopen(name.c_str(), mode.c_str());
			if (dump_file == NULL)
			{
				std::ostringstream errstr;
				errstr << "Restart file could not be opened: " << name;
				this->ErrorHandler(IRM_FAIL, errstr.str());
			}
		}
	}
	catch (...)
	{
		return_value = IRM_FAIL;
	}

	// Return on error opening dump file
	MPI_Bcast(&return_value, 1, MPI_INT, 0, phreeqcrm_comm);
	if (return_value != IRM_OK)
	{
		return this->ReturnHandler(return_value, "PhreeqcRM::DumpModule");
	}
	int block = 5000;
	// Calculate max
	int max = 0;
	for (int n = 0; n < mpi_tasks; n++)
	{
		int count = this->end_cell[n] - this->start_cell[n] + 1;
		max = count > max ? count : max;
	}
	
	int nblocks = max / block;
	if (max % block > 0) nblocks += 1;


	std::vector<char> char_buffer;
	const size_t gzblock = 4094;
	char buffer[gzblock+2];
	int total_cells = this->end_cell[this->mpi_tasks -1];
	if (total_cells <= 0) total_cells = 1;
	int pct = 10;
	int block_count = 0;
	if (mpi_myself == 0)
	{
		//std::cerr << "Dump 0% ";
		std::ostringstream msg;
		this->ScreenMessage("Dump 0% ");
	}
	// Try for dumping data
	try
	{
		// write dump file data
		this->workers[0]->SetDumpStringOn(true);

		for (int iblock = 0; iblock < nblocks; iblock++)
		{
			for (int n = 0; n < this->mpi_tasks; n++)
			{
				int begin = this->start_cell[n] + iblock * block;
				if (begin <= this->end_cell[n]) 
				{
					int last = this->start_cell[n] + (iblock + 1) * block - 1;
					if (last > this->end_cell[n]) last = this->end_cell[n];
					if (mpi_myself == 0)
					{
						block_count += last - begin + 1;
					}
					if (this->mpi_myself == n)
					{		
						// Dump block of cells
						std::ostringstream in;
						in << "DUMP; -cells " << begin << "-" << last << "\n";
						int status = this->workers[0]->RunString(in.str().c_str());
						if (status != 0)
						{
							this->ErrorMessage(this->workers[0]->GetErrorString());
						}
						this->ErrorHandler(PhreeqcRM::Int2IrmResult(status, false), "RunString");
					}
				}
			}
			if (mpi_myself == 0)
			{
				size_t dump_length = strlen(this->GetWorkers()[0]->GetDumpString());
				const char * start = this->GetWorkers()[0]->GetDumpString();
				const char * end = &this->GetWorkers()[0]->GetDumpString()[dump_length];
				for (const char * ptr = start; ptr <  end; ptr += gzblock)
				{
					strncpy(buffer, ptr, gzblock);
					buffer[gzblock] = '\0';
					int err = gzprintf(dump_file, "%s", buffer);
					if (err <= 0)
					{
						this->ErrorHandler(IRM_FAIL, "gzprintf");
					}
				}
			}
			for (int n = 1; n < this->mpi_tasks; n++)
			{
				// Need to transfer output stream to root and print

				if (mpi_myself == n)
				{
					int size = (int) strlen(this->workers[0]->GetDumpString());
					MPI_Send(&size, 1, MPI_INT, 0, 0, phreeqcrm_comm);
					MPI_Send((void *) this->workers[0]->GetDumpString(), size, MPI_CHAR, 0, 0, phreeqcrm_comm);
				}
				else if (this->mpi_myself == 0)
				{
					MPI_Status mpi_status;
					int size;
					MPI_Recv(&size, 1, MPI_INT, n, 0, phreeqcrm_comm, &mpi_status);
					char_buffer.resize(size+1);
					MPI_Recv((void *) &char_buffer.front(), size, MPI_CHAR, n, 0, phreeqcrm_comm, &mpi_status);
					char_buffer[size] = '\0';

					char * start = &char_buffer.front();
					char * end = &char_buffer[size];
					for (const char * ptr = start; ptr <  end; ptr += gzblock)
					{
						strncpy(buffer, ptr, gzblock);
						buffer[gzblock] = '\0';
						int err = gzprintf(dump_file, "%s", buffer);
						if (err <= 0)
						{
							this->ErrorHandler(IRM_FAIL, "gzprintf");
						}
					}
				}
			}
			if (mpi_myself == 0 && block_count * 100 / total_cells > pct)
			{
				int pct_block_count = (block_count * 10 / total_cells) * 10;
				if (pct_block_count < 100)
				{
					//std::cerr << pct_block_count << "% ";
					std::ostringstream msg;
					msg << pct_block_count << "% ";
					this->ScreenMessage(msg.str().c_str());
				}
				pct = pct_block_count + 10;
			}
			MPI_Barrier(phreeqcrm_comm);
		}
		if (mpi_myself == 0)
		{
			std::cerr << "100% " << std::endl;
		}

		// Clear dump string to save space
		std::ostringstream clr;
		clr << "END\n";
		{
			int status;
			status = this->GetWorkers()[0]->RunString(clr.str().c_str());
			if (status != 0)
			{
				this->ErrorMessage(this->workers[0]->GetErrorString());
			}
		}
	}
	catch (...)
	{
		return_value = IRM_FAIL;
	}
	if (mpi_myself == 0)
	{
		gzclose(dump_file);
	}

	return this->ReturnHandler(return_value, "PhreeqcRM::DumpModule");
}
#endif
#else // MPI
#ifdef NEWDUMP
/* ---------------------------------------------------------------------- */
IRM_RESULT
PhreeqcRM::DumpModule(bool dump_on, bool append)
/* ---------------------------------------------------------------------- */
{
	this->phreeqcrm_error_string.clear();
	if (!dump_on) return IRM_OK;

	IRM_RESULT return_value = IRM_OK;

	// Open file on root
	try
	{
		// open dump file
		gzFile dump_file;
		std::string name(this->dump_file_name);

		// open
		std::string mode;
#ifdef USE_GZ
		mode = append ? "ab1" : "wb1";
#else
		mode = append ? "a" : "w";
#endif
		dump_file = gzopen(name.c_str(), mode.c_str());
		if (dump_file == NULL)
		{
			std::ostringstream errstr;
			errstr << "Restart file could not be opened: " << name;
			this->ErrorHandler(IRM_FAIL, errstr.str());
		}


		std::vector<cxxStorageBin> sz_uz;
		sz_uz.resize(nthreads);
		for (int n = 0; n < nthreads; n++)
		{
			this->workers[n]->Get_PhreeqcPtr()->phreeqc2cxxStorageBin(sz_uz[n]);
			if (this->partition_uz_solids)
			{
				sz_uz[n].Add_uz(this->workers[n]->uz_bin);
			}
		}

		int block = 10000;
		// Calculate max
		int max = 0;
		for (int n = 0; n < nthreads; n++)
		{
			int count = this->end_cell[n] - this->start_cell[n] + 1;
			max = count > max ? count : max;
		}

		int nblocks = max / block;
		if (max % block > 0) nblocks += 1;

		std::vector<char> char_buffer;
		const size_t gzblock = 4094;
		char buffer[gzblock + 2];
		int total_cells = this->end_cell[this->nthreads - 1];
		if (total_cells <= 0) total_cells = 1;
		int pct = 10;
		int block_count = 0;

		//std::cerr << "Dump 0% ";
		this->ScreenMessage("Dump 0% ");

		for (int iblock = 0; iblock < nblocks; iblock++)
		{
			for (int n = 0; n < this->nthreads; n++)
			{
				// count cells for blocks
				int begin = this->start_cell[n] + iblock * block;
				if (begin <= this->end_cell[n])
				{
					int last = this->start_cell[n] + (iblock + 1) * block - 1;
					if (last > this->end_cell[n]) last = this->end_cell[n];
					block_count += last - begin + 1;
				}
			}

			std::vector<std::string> dump_strings;
			dump_strings.resize(this->nthreads);
#ifdef USE_OPENMP
			omp_set_num_threads(this->nthreads);
#pragma omp parallel
#pragma omp for
#endif
			for (int n = 0; n < this->nthreads; n++)
			{
				std::ostringstream oss;
				int begin = this->start_cell[n] + iblock * block;
				if (begin <= this->end_cell[n])
				{
					int last = this->start_cell[n] + (iblock + 1) * block - 1;
					if (last > this->end_cell[n]) last = this->end_cell[n];
					// Dump block of cells
					sz_uz[n].dump_raw_range(oss, begin, last, 2);
					dump_strings[n] = oss.str();
				}
			}
			for (int n = 0; n < this->nthreads; n++)
			{
				// Write data to file
				size_t dump_length = strlen(dump_strings[n].c_str());
				const char * start = dump_strings[n].c_str();
				const char * end = &(dump_strings[n].c_str()[dump_length]);
				for (const char * ptr = start; ptr < end; ptr += gzblock)
				{
					strncpy(buffer, ptr, gzblock);
					buffer[gzblock] = '\0';
					int err = gzprintf(dump_file, "%s", buffer);
					if (err <= 0)
					{
						this->ErrorHandler(IRM_FAIL, "gzprintf");
					}
				}
			}
			//for (int n = 0; n < this->nthreads; n++)
			//{
			//	// Write data to file
			//	size_t dump_length = strlen(this->GetWorkers()[n]->GetDumpString());
			//	const char * start = this->GetWorkers()[n]->GetDumpString();
			//	const char * end = &this->GetWorkers()[n]->GetDumpString()[dump_length];
			//	for (const char * ptr = start; ptr < end; ptr += gzblock)
			//	{
			//		strncpy(buffer, ptr, gzblock);
			//		buffer[gzblock] = '\0';
			//		int err = gzprintf(dump_file, "%s", buffer);
			//		if (err <= 0)
			//		{
			//			this->ErrorHandler(IRM_FAIL, "gzprintf");
			//		}
			//	}
			//}
			int pct_block_count = (block_count * 10 / total_cells) * 10;
			if (block_count * 100 / total_cells > pct)
			{
				if (pct_block_count < 100)
				{
					//std::cerr << pct_block_count << "% ";
					std::ostringstream msg;
					msg << pct_block_count << "% ";
					this->ScreenMessage(msg.str().c_str());
				}
				pct = pct_block_count + 10;
			}
		}

		{
			//std::cerr << "100% " << std::endl;
			std::ostringstream msg;
			msg << "100% " << std::endl;
			this->ScreenMessage(msg.str().c_str());
		}

		for (int n = 0; n < this->nthreads; n++)
		{
			// Clear dump string to save space
			std::ostringstream clr;
			clr << "END\n";
			{
				int status;
				status = this->GetWorkers()[n]->RunString(clr.str().c_str());
				if (status != 0)
				{
					this->ErrorMessage(this->workers[n]->GetErrorString());
				}
			}
		}
		gzclose(dump_file);
	}
	catch (...)
	{
		return_value = IRM_FAIL;
	}
	return this->ReturnHandler(return_value, "PhreeqcRM::DumpModule");
}
#endif
#ifdef ORIGINALDUMP
/* ---------------------------------------------------------------------- */
IRM_RESULT
PhreeqcRM::DumpModule(bool dump_on, bool append)
/* ---------------------------------------------------------------------- */
{
	this->phreeqcrm_error_string.clear();
	if (!dump_on) return IRM_OK;

	IRM_RESULT return_value = IRM_OK;

	// Open file on root
	try
	{
		// open dump file
		gzFile dump_file;
		std::string name(this->dump_file_name);

		// open
		std::string mode;
#ifdef USE_GZ
		mode = append ? "ab1" : "wb1";
#else
		mode = append ? "a" : "w";
#endif
		dump_file = gzopen(name.c_str(), mode.c_str());
		if (dump_file == NULL)
		{
			std::ostringstream errstr;
			errstr << "Restart file could not be opened: " << name;
			this->ErrorHandler(IRM_FAIL, errstr.str());
		}

		int block = 10000;
		// Calculate max
		int max = 0;
		for (int n = 0; n < nthreads; n++)
		{
			int count = this->end_cell[n] - this->start_cell[n] + 1;
			max = count > max ? count : max;
		}

		int nblocks = max / block;
		if (max % block > 0) nblocks += 1;

		std::vector<char> char_buffer;
		const size_t gzblock = 4094;
		char buffer[gzblock+2];
		int total_cells = this->end_cell[this->nthreads -1];
		if (total_cells <= 0) total_cells = 1;
		int pct = 10;
		int block_count = 0;

		//std::cerr << "Dump 0% ";
		this->ScreenMessage("Dump 0% ");

		for (int iblock = 0; iblock < nblocks; iblock++)
		{
			for (int n = 0; n < this->nthreads; n++)
			{
				// count cells for blocks
				int begin = this->start_cell[n] + iblock * block;
				if (begin <= this->end_cell[n]) 
				{
					int last = this->start_cell[n] + (iblock + 1) * block - 1;
					if (last > this->end_cell[n]) last = this->end_cell[n];
					block_count += last - begin + 1;
				}
			}
#ifdef USE_OPENMP
		omp_set_num_threads(this->nthreads);
#pragma omp parallel
#pragma omp for
#endif
			for (int n = 0; n < this->nthreads; n++)
			{
				// write dump file data
				this->workers[n]->SetDumpStringOn(true);
				int begin = this->start_cell[n] + iblock * block;
				if (begin <= this->end_cell[n]) 
				{
					int last = this->start_cell[n] + (iblock + 1) * block - 1;
					if (last > this->end_cell[n]) last = this->end_cell[n];
					// Dump block of cells
					std::ostringstream in;
					in << "DUMP; -cells " << begin << "-" << last << "\n";
					int status = this->workers[n]->RunString(in.str().c_str());
					if (status != 0)
					{
						this->ErrorMessage(this->workers[n]->GetErrorString());
					}
					this->ErrorHandler(PhreeqcRM::Int2IrmResult(status, false), "RunString");
				}
			}
			for (int n = 0; n < this->nthreads; n++)
			{
				// Write data to file
				size_t dump_length = strlen(this->GetWorkers()[n]->GetDumpString());
				const char * start = this->GetWorkers()[n]->GetDumpString();
				const char * end = &this->GetWorkers()[n]->GetDumpString()[dump_length];
				for (const char * ptr = start; ptr <  end; ptr += gzblock)
				{
					strncpy(buffer, ptr, gzblock);
					buffer[gzblock] = '\0';
					int err = gzprintf(dump_file, "%s", buffer);
					if (err <= 0)
					{
						this->ErrorHandler(IRM_FAIL, "gzprintf");
					}
				}
			}
			int pct_block_count = (block_count * 10 / total_cells) * 10;
			if (block_count * 100 / total_cells > pct)
			{
				if (pct_block_count < 100)
				{
					//std::cerr << pct_block_count << "% ";
					std::ostringstream msg;
					msg << pct_block_count << "% ";
					this->ScreenMessage(msg.str().c_str());
				}
				pct = pct_block_count + 10;
			}
		}
		{
			//std::cerr << "100% " << std::endl;
			std::ostringstream msg;
			msg << "100% " << std::endl;
			this->ScreenMessage(msg.str().c_str());
		}

		for (int n = 0; n < this->nthreads; n++)
		{
			// Clear dump string to save space
			std::ostringstream clr;
			clr << "END\n";
			{
				int status;
				status = this->GetWorkers()[n]->RunString(clr.str().c_str());
				if (status != 0)
				{
					this->ErrorMessage(this->workers[n]->GetErrorString());
				}
			}
		}
		gzclose(dump_file);
	}
	catch (...)
	{
		return_value = IRM_FAIL;
	}
	return this->ReturnHandler(return_value, "PhreeqcRM::DumpModule");
}
#endif
#endif // MPI
/* ---------------------------------------------------------------------- */
void
PhreeqcRM::ErrorMessage(const std::string &error_string, bool prepend)
/* ---------------------------------------------------------------------- */
{
#ifdef USE_OPENMP
#pragma omp critical
#endif
	{
		std::ostringstream estr;
		if (prepend)
			estr << "ERROR: ";
		estr << error_string << std::endl;
		this->phreeqcrm_error_string.append(estr.str().c_str());
		this->phreeqcrm_io->output_msg(estr.str().c_str());
		this->phreeqcrm_io->log_msg(estr.str().c_str());
		this->phreeqcrm_io->error_msg(estr.str().c_str());
	}
}
/* ---------------------------------------------------------------------- */
bool
PhreeqcRM::FileExists(const std::string &name)
/* ---------------------------------------------------------------------- */
{
	FILE *stream;
	if ((stream = fopen(name.c_str(), "r")) == NULL)
	{
		return false;				/* doesn't exist */
	}
	fclose(stream);
	return true;					/* exists */
}

/* ---------------------------------------------------------------------- */
void
PhreeqcRM::FileRename(const std::string &temp_name, const std::string &name,
	const std::string &backup_name)
/* ---------------------------------------------------------------------- */
{
	if (PhreeqcRM::FileExists(name))
	{
		if (PhreeqcRM::FileExists(backup_name.c_str()))
			remove(backup_name.c_str());
		(void)rename(name.c_str(), backup_name.c_str());
	}
	(void)rename(temp_name.c_str(), name.c_str());
}
/* ---------------------------------------------------------------------- */
int
PhreeqcRM::FindComponents(void)
/* ---------------------------------------------------------------------- */
{
/*
 *   Counts components in any defined solution, gas_phase, exchanger,
 *   surface, or pure_phase_assemblage
 *
 *   Returns
 *		n_comp, which is total, including H, O, elements, and Charge
 *      names, which contains character strings with names of components
 */
	bool clear = false;
	this->phreeqcrm_error_string.clear();
	try
	{
#ifdef USE_MPI
		if (this->mpi_myself == 0)
		{
			int method = METHOD_FINDCOMPONENTS;
			MPI_Bcast(&method, 1, MPI_INT, 0, phreeqcrm_comm);
		}
#endif
		// Always include H, O, Charge

		std::set<std::string> component_set;

		size_t fixed_components = 3;
		if (this->component_h2o)
			fixed_components = 4;

		// save old components
		for (size_t i = fixed_components; i < this->components.size(); i++)
		{
			component_set.insert(this->components[i]);
		}

		// Get other components
		IPhreeqcPhast * phast_iphreeqc_worker = this->GetWorkers()[this->nthreads];
		size_t count_components = phast_iphreeqc_worker->GetComponentCount();

		size_t i;
		for (i = 0; i < count_components; i++)
		{
			std::string comp(phast_iphreeqc_worker->GetComponent((int) i));
			assert (comp != "H");
			assert (comp != "O");
			assert (comp != "Charge");
			assert (comp != "charge");

			component_set.insert(comp);
		}
		// clear and refill components in vector
		this->components.clear();

		// Always include H, O, Charge
		if (this->component_h2o)
			this->components.push_back("H2O");
		this->components.push_back("H");
		this->components.push_back("O");
		this->components.push_back("Charge");
		for (std::set<std::string>::iterator it = component_set.begin(); it != component_set.end(); it++)
		{
			this->components.push_back(*it);
		}
		// Calculate gfw for components
		this->gfw.clear();
		for (i = 0; i < components.size(); i++)
		{
			if (components[i] == "Charge")
			{
				this->gfw.push_back(1.0);
			}
			else
			{
				this->gfw.push_back(phast_iphreeqc_worker->Get_gfw(components[i].c_str()));
			}
		}
		// Get list of species
		if (this->species_save_on)
		{
			phast_iphreeqc_worker->PhreeqcPtr->save_species = true;
		}
		// Make lists regardless of species_save_on
		{
			int next = phast_iphreeqc_worker->PhreeqcPtr->next_user_number(Keywords::KEY_SOLUTION);
			{
				std::ostringstream in;
				in << "SOLUTION " << next << "\n";
				if (phast_iphreeqc_worker->PhreeqcPtr->llnl_temp.size() > 0)
				{
					in << "-temp " << phast_iphreeqc_worker->PhreeqcPtr->llnl_temp[0] << ";";
				}
				for (i = 0; i < components.size(); i++)
				{
					if (components[i] == "H") continue;
					if (components[i] == "O") continue;
					if (components[i] == "H2O") continue;
					if (components[i] == "Charge") continue;
					in << components[i] << " 1e-6\n";
				}
				int status = phast_iphreeqc_worker->RunString(in.str().c_str());
				if (status != 0)
				{
					this->ErrorMessage(phast_iphreeqc_worker->GetErrorString());
					throw PhreeqcRMStop();
				}
			}
			species_names.clear();
			species_z.clear();
			s_num2rm_species_num.clear();
			species_stoichiometry.clear();
			species_d_25.clear();
			for (int i = 0; i < (int)phast_iphreeqc_worker->PhreeqcPtr->s_x.size(); i++)
			{
				species_names.push_back(phast_iphreeqc_worker->PhreeqcPtr->s_x[i]->name);
				species_z.push_back(phast_iphreeqc_worker->PhreeqcPtr->s_x[i]->z);
				species_d_25.push_back(phast_iphreeqc_worker->PhreeqcPtr->s_x[i]->dw);
				s_num2rm_species_num[phast_iphreeqc_worker->PhreeqcPtr->s_x[i]->number] = i;
				cxxNameDouble nd(phast_iphreeqc_worker->PhreeqcPtr->s_x[i]->next_elt);
				nd.add("Charge", phast_iphreeqc_worker->PhreeqcPtr->s_x[i]->z);
				species_stoichiometry.push_back(nd);
			}
			ElementRedoxSet.clear();
			for (size_t i = 0; i < phast_iphreeqc_worker->PhreeqcPtr->master.size(); i++)
			{
				if (phast_iphreeqc_worker->PhreeqcPtr->master[i]->in != FALSE)
				{
					std::string e = phast_iphreeqc_worker->PhreeqcPtr->master[i]->elt->name;
					if (e != "E")
					{
						ElementRedoxSet.insert(e);
					}
					e = phast_iphreeqc_worker->PhreeqcPtr->master[i]->elt->primary->elt->name;
					if (e != "E")
					{
						ElementRedoxSet.insert(e);
					}
				}
			}
			for (int i = 0; i < (int)phast_iphreeqc_worker->PhreeqcPtr->phases.size(); i++)
			{
				if (phast_iphreeqc_worker->PhreeqcPtr->phases[i]->in == TRUE)
				{
					SINamesList.push_back(phast_iphreeqc_worker->PhreeqcPtr->phases[i]->name);
				}
			}
			{
				std::ostringstream in;
				in << "DELETE; -solution " << next << "\n";
				int status = phast_iphreeqc_worker->RunString(in.str().c_str());
				if (status != 0)
				{
					this->ErrorMessage(phast_iphreeqc_worker->GetErrorString());
					throw PhreeqcRMStop();
				}
			}
		}
		// Make all lists
		{
			// Make set for surfaces
			std::set<std::string> surface_types_set;
			std::map<std::string, std::string> surface_names_map;
			if (!clear)
			{
				for (size_t ii = 0; ii < this->SurfaceSpeciesNamesList.size(); ii++)
				{
					surface_types_set.insert(this->SurfaceTypesList[ii]);
					surface_names_map[this->SurfaceTypesList[ii]] = this->SurfaceNamesList[ii];
				}
			}
			// add new surface types 
			{
				const std::list<std::string> &surftype = phast_iphreeqc_worker->GetSurfaceTypeList();
				const std::list<std::string> &surfnames = phast_iphreeqc_worker->GetSurfaceNamesList();
				{
					std::list<std::string>::const_iterator surftype_it = surftype.begin();
					std::list<std::string>::const_iterator surfnames_it = surfnames.begin();
					for (; surftype_it != surftype.end(); surftype_it++)
					{
						surface_types_set.insert(*surftype_it);
						surface_names_map[*surftype_it] = *surfnames_it++;
					}
				}
			}
			// make set for exchange
			std::set<std::string> ex_set;
			if (!clear)
			{
				for (size_t ii = 0; ii < this->ExchangeNamesList.size(); ii++)
				{
					ex_set.insert(this->ExchangeNamesList[ii]);
				}
			}
			// add new exchange sites
			{
				const std::list<std::string> &ex = phast_iphreeqc_worker->GetExchangeNamesList();
				{
					std::list<std::string>::const_iterator ex_it = ex.begin();
					for (; ex_it != ex.end(); ex_it++)
					{
						ex_set.insert(*ex_it);
					}
				}
			}
			// write solution
			int next = phast_iphreeqc_worker->PhreeqcPtr->next_user_number(Keywords::KEY_SOLUTION);
			if (ex_set.size() > 0 || surface_types_set.size() > 0)
			{
				std::ostringstream in;
				in << "SOLUTION " << next << "\n";
				if (phast_iphreeqc_worker->PhreeqcPtr->llnl_temp.size() > 0)
				{
					in << "-temp " << phast_iphreeqc_worker->PhreeqcPtr->llnl_temp[0] << ";";
				}
				for (i = 0; i < components.size(); i++)
				{
					if (components[i] == "H") continue;
					if (components[i] == "O") continue;
					if (components[i] == "H2O") continue;
					if (components[i] == "Charge") continue;
					in << components[i] << " 1e-6\n";
				}
				in << "END\n";
				int status = phast_iphreeqc_worker->RunString(in.str().c_str());
				if (status != 0)
				{
					this->ErrorMessage(phast_iphreeqc_worker->GetErrorString());
					throw PhreeqcRMStop();
				}
			}
			// write surface and save vectors
			int next_surf = phast_iphreeqc_worker->PhreeqcPtr->next_user_number(Keywords::KEY_SURFACE);
			this->SurfaceSpeciesNamesList.clear();
			this->SurfaceTypesList.clear();
			this->SurfaceNamesList.clear();
			if (surface_types_set.size() > 0)
			{
				std::set<std::string>::iterator cit = surface_types_set.begin();
				for (; cit != surface_types_set.end(); cit++)
				{
					std::ostringstream in;
					in << "SURFACE " << next_surf << "\n";
					in << "  -eq " << next << "\n";
					in << "  " << *cit << "  0.001  1   1\n";
					int status = phast_iphreeqc_worker->RunString(in.str().c_str());
					if (status != 0)
					{
						this->ErrorMessage(phast_iphreeqc_worker->GetErrorString());
						throw PhreeqcRMStop();
					}
					// fill surface vectors
					for (int i = 0; i < (int)phast_iphreeqc_worker->PhreeqcPtr->s_x.size(); i++)
					{
						if (phast_iphreeqc_worker->PhreeqcPtr->s_x[i]->type == SURF)
						{
							this->SurfaceSpeciesNamesList.push_back(phast_iphreeqc_worker->PhreeqcPtr->s_x[i]->name);
							this->SurfaceTypesList.push_back(*cit);
							this->SurfaceNamesList.push_back(surface_names_map[*cit]);
						}
					}
					{
						std::ostringstream in1;
						in1 << "DELETE -surface " << next_surf << "\n";
						status = phast_iphreeqc_worker->RunString(in1.str().c_str());
						if (status != 0)
						{
							this->ErrorMessage(phast_iphreeqc_worker->GetErrorString());
							throw PhreeqcRMStop();
						}
					}
				}
			}
			// Exchange species
			int next_ex = phast_iphreeqc_worker->PhreeqcPtr->next_user_number(Keywords::KEY_EXCHANGE);
			this->ExchangeSpeciesNamesList.clear();
			this->ExchangeNamesList.clear();
			if (ex_set.size() > 0)
			{
				std::set<std::string>::iterator cit = ex_set.begin();
				for (; cit != ex_set.end(); cit++)
				{
					std::ostringstream in;
					in << "EXCHANGE " << next_ex << "\n";
					in << "  -eq " << next << "\n";
					in << "  " << *cit << "  0.001\n";
					int status = phast_iphreeqc_worker->RunString(in.str().c_str());
					if (status != 0)
					{
						this->ErrorMessage(phast_iphreeqc_worker->GetErrorString());
						throw PhreeqcRMStop();
					}
					for (int i = 0; i < (int)phast_iphreeqc_worker->PhreeqcPtr->s_x.size(); i++)
					{
						if (phast_iphreeqc_worker->PhreeqcPtr->s_x[i]->type == EX)
						{
							this->ExchangeSpeciesNamesList.push_back(phast_iphreeqc_worker->PhreeqcPtr->s_x[i]->name);
							this->ExchangeNamesList.push_back(*cit);
						}
					}
					{
						std::ostringstream in1;
						in1 << "DELETE -exchange " << next_ex << "\n";
						status = phast_iphreeqc_worker->RunString(in1.str().c_str());
						if (status != 0)
						{
							this->ErrorMessage(phast_iphreeqc_worker->GetErrorString());
							throw PhreeqcRMStop();
						}
					}
				}
			}
			if (ex_set.size() > 0 || surface_types_set.size() > 0)
			{
				std::ostringstream in;
				in << "DELETE; -solution " << next << "\n";
				int status = phast_iphreeqc_worker->RunString(in.str().c_str());
				if (status != 0)
				{
					this->ErrorMessage(phast_iphreeqc_worker->GetErrorString());
					throw PhreeqcRMStop();
				}
			}
			// equilibrium_phases
			{
				// Move to set
				std::set<std::string> eq_set;
				if (!clear)
				{
					for (size_t i = 0; i < this->EquilibriumPhasesList.size(); i++)
					{
						eq_set.insert(this->EquilibriumPhasesList[i]);
					}
				}
				// add new equilibrium phases to set
				std::list<std::string>::const_iterator cit = phast_iphreeqc_worker->GetEquilibriumPhasesList().begin();
				for (; cit != phast_iphreeqc_worker->GetEquilibriumPhasesList().end(); cit++)
				{
					eq_set.insert(*cit);
				}
				// move set to vector
				this->EquilibriumPhasesList.clear();
				std::set<std::string>::const_iterator eqit = eq_set.begin();
				for (; eqit != eq_set.end(); eqit++)
				{
					this->EquilibriumPhasesList.push_back(*eqit);
				}
			}
			// gas phase components
			{				
				// Move to set
				std::set<std::string> g_set;
				if (!clear)
				{
					for (size_t i = 0; i < this->GasComponentsList.size(); i++)
					{
						g_set.insert(this->GasComponentsList[i]);
					}
				}
				// add new gas components to set
				std::list<std::string>::const_iterator cit = phast_iphreeqc_worker->GetGasComponentsList().begin();
				for (; cit != phast_iphreeqc_worker->GetGasComponentsList().end(); cit++)
				{
					g_set.insert(*cit);
				}
				// move set to vector
				this->GasComponentsList.clear();
				std::set<std::string>::const_iterator git = g_set.begin();
				for (; git != g_set.end(); git++)
				{
					this->GasComponentsList.push_back(*git);
				}
			}
			// Kinetics
			{
				// Move to set
				std::set<std::string> k_set;
				if (!clear)
				{
					for (size_t i = 0; i < this->KineticReactionsList.size(); i++)
					{
						k_set.insert(this->KineticReactionsList[i]);
					}
				}
				// add new kinetic reactions to set
				std::list<std::string>::const_iterator cit = phast_iphreeqc_worker->GetKineticReactionsList().begin();
				for (; cit != phast_iphreeqc_worker->GetKineticReactionsList().end(); cit++)
				{
					k_set.insert(*cit);
				}
				// move set to vector
				this->KineticReactionsList.clear();
				std::set<std::string>::const_iterator kit = k_set.begin();
				for (; kit != k_set.end(); kit++)
				{
					this->KineticReactionsList.push_back(*kit);
				}
			}
			// Solid solutions
			{
				// move existing component names to set and solid solution names to map
				std::set<std::string> sscomp_set;
				std::map<std::string, std::string> ssnames_map;
				if (!clear)
				{
					for (size_t i = 0; i < this->SolidSolutionComponentsList.size(); i++)
					{
						sscomp_set.insert(this->SolidSolutionComponentsList[i]);
						ssnames_map[this->SolidSolutionComponentsList[i]] = this->SolidSolutionNamesList[i];
					}
				}
				// add new component names set and solid solution names to map
				std::list<std::string>::const_iterator cit = phast_iphreeqc_worker->GetSolidSolutionComponentsList().begin();
				std::list<std::string>::const_iterator nit = phast_iphreeqc_worker->GetSolidSolutionNamesList().begin();
				for (; cit != phast_iphreeqc_worker->GetSolidSolutionComponentsList().end(); cit++)
				{
					if (sscomp_set.find(*cit) == sscomp_set.end())
					{
						sscomp_set.insert(*cit);
						ssnames_map[*cit] = *nit++;
					}
				}
				// Move from set and map to vectors
				this->SolidSolutionComponentsList.clear();
				this->SolidSolutionNamesList.clear();
				std::set<std::string>::const_iterator set_it = sscomp_set.begin();
				for (; set_it != sscomp_set.end(); set_it++)
				{
					this->SolidSolutionComponentsList.push_back(*set_it);
					this->SolidSolutionNamesList.push_back(ssnames_map[*set_it]);
				}
			}
		}
	}
	catch (...)
	{
		return this->ReturnHandler(IRM_FAIL, "PhreeqcRM::FindComponents");
	}
	this->GenerateAutoOutputVars();
	return (int) this->components.size();
}
/* ---------------------------------------------------------------------- */
void
PhreeqcRM::GatherNchem(std::vector<double> &source, std::vector<double> &destination)
/* ---------------------------------------------------------------------- */
{
	// source is nchem pieces on workers
	// destination is nxyz for root only
#ifdef USE_MPI
	int * recv_counts = NULL;
	int * recv_displs = NULL;
	int send_count;

	send_count = end_cell[this->mpi_myself] - start_cell[this->mpi_myself] + 1;
	recv_counts = new int[this->mpi_tasks];
	recv_displs = new int[this->mpi_tasks];
	for (int j = 0; j < this->mpi_tasks; j++)
	{
		recv_counts[j] = end_cell[j] - start_cell[j] + 1;
		recv_displs[j] = start_cell[j];
	}
	std::vector<double> dbuffer;
	if (this->mpi_myself == 0)
	{
		dbuffer.resize(this->count_chemistry);
	}
	else
	{
		dbuffer.resize(1);
	}

	MPI_Gatherv((void *) &(source[0]), send_count, MPI_DOUBLE, &dbuffer[0], recv_counts, recv_displs, MPI_DOUBLE, 0, this->phreeqcrm_comm);

	// Place in tempc on root
	if (mpi_myself == 0)
	{
		destination.resize(this->nxyz);

		for(size_t i = 0; i < (size_t) this->count_chemistry; i++)
		{
			for(size_t j = 0; j < backward_mapping[i].size(); j++)
			{
				int n = backward_mapping[i][j];
				destination[n] = dbuffer[i];
			}
		}
	}
	delete [] recv_counts;
	delete [] recv_displs;
#endif
}
/* ---------------------------------------------------------------------- */
void 
PhreeqcRM::GenerateAutoOutputVars()
/* ---------------------------------------------------------------------- */
{
	// no-op
}
/* ---------------------------------------------------------------------- */
IRM_RESULT
PhreeqcRM::GetIthConcentration(int i, std::vector<double>& c)
/* ---------------------------------------------------------------------- */
{
	this->phreeqcrm_error_string.clear();
	try
	{
		if (i >= 0 && i < this->GetComponentCount())
		{
			if (this->CurrentConcentrations.size() != this->GetGridCellCount() * this->GetComponentCount())
			{
				this->GetConcentrations(this->CurrentConcentrations);
			}
			int nxyz = this->GetGridCellCount();
			c.resize(nxyz);
			for (int j = 0; j < nxyz; j++)
			{
				c[j] = CurrentConcentrations[i * nxyz + j];
			}
			return IRM_OK;
		}
	}
	catch (...)
	{
	}
	return this->ReturnHandler(IRM_INVALIDARG, "PhreeqcRM::GetIthConcentration");
}
/* ---------------------------------------------------------------------- */
IRM_RESULT
PhreeqcRM::SetIthConcentration(int i, std::vector<double>& c)
/* ---------------------------------------------------------------------- */
{
	this->phreeqcrm_error_string.clear();
	try
	{
		if (i >= 0 && i < this->GetComponentCount())
		{
			if (this->IthCurrentConcentrations.size() != this->GetGridCellCount()*this->GetComponentCount())
			{
				this->IthCurrentConcentrations.clear();
				this->IthCurrentConcentrations.resize(this->GetGridCellCount() * this->GetComponentCount(), 0);
				this->IthConcentrationSet.clear();
			}
			int nxyz = this->GetGridCellCount();
			assert(c.size() == nxyz);
			for (int j = 0; j < nxyz; j++)
			{
				IthCurrentConcentrations[i * nxyz + j] = c[j];
			}
			IthConcentrationSet.insert(i);
			return IRM_OK;
		}
	}
	catch (...)
	{
	}
	return this->ReturnHandler(IRM_INVALIDARG, "PhreeqcRM::GetIthConcentration");
}
/* ---------------------------------------------------------------------- */
IRM_RESULT
PhreeqcRM::GetIthSpeciesConcentration(int i, std::vector<double>& c)
/* ---------------------------------------------------------------------- */
{
	this->phreeqcrm_error_string.clear();
	try
	{
		if (species_save_on && i >= 0 && i < this->GetSpeciesCount())
		{
			if (this->CurrentSpeciesConcentrations.size() != this->GetGridCellCount() * this->GetSpeciesCount())
			{
				this->GetSpeciesConcentrations(this->CurrentSpeciesConcentrations);
			}
			int nxyz = this->GetGridCellCount();
			c.resize(nxyz);
			for (int j = 0; j < nxyz; j++)
			{
				c[j] = CurrentSpeciesConcentrations[i * nxyz + j];
			}
			return IRM_OK;
		}
	}
	catch (...)
	{
	}
	return this->ReturnHandler(IRM_INVALIDARG, "PhreeqcRM::GetIthSpeciesConcentration");
}
/* ---------------------------------------------------------------------- */
IRM_RESULT
PhreeqcRM::SetIthSpeciesConcentration(int i, std::vector<double>& c)
/* ---------------------------------------------------------------------- */
{
	this->phreeqcrm_error_string.clear();
	try
	{
		if (i >= 0 && i < this->GetSpeciesCount())
		{
			if (this->IthCurrentSpeciesConcentrations.size() != this->GetGridCellCount() * this->GetSpeciesCount())
			{
				this->IthCurrentSpeciesConcentrations.clear();
				this->IthCurrentSpeciesConcentrations.resize(this->GetGridCellCount() * this->GetSpeciesCount(), 0);
				this->IthSpeciesConcentrationSet.clear();
			}
			int nxyz = this->GetGridCellCount();
			assert(c.size() == nxyz);
			for (int j = 0; j < nxyz; j++)
			{
				IthCurrentSpeciesConcentrations[i * nxyz + j] = c[j];
			}
			IthSpeciesConcentrationSet.insert(i);
			return IRM_OK;
		}
	}
	catch (...)
	{
	}
	return this->ReturnHandler(IRM_INVALIDARG, "PhreeqcRM::GetIthConcentration");
}

void PhreeqcRM::GetBackwardMappingSWIG(std::vector<int>& nback_output, std::vector<int>& cellnumbers_output)
{
	nback_output.clear();
	cellnumbers_output.clear();
	std::vector <std::vector<int> > back = this->GetBackwardMapping();
	for (size_t i = 0; i < back.size(); i++)
	{
		nback_output.push_back((int)back[i].size());
		for (size_t j = 0; j < back[i].size(); j++)
		{
			cellnumbers_output.push_back(back[i][j]);
		}
	}
}
#ifdef USE_MPI
/* ---------------------------------------------------------------------- */
IRM_RESULT
PhreeqcRM::GetConcentrations(std::vector<double> &c)
/* ---------------------------------------------------------------------- */
{
	this->phreeqcrm_error_string.clear();
	IRM_RESULT return_value = IRM_OK;
	try
	{
		if (this->mpi_myself == 0)
		{
			int method = METHOD_GETCONCENTRATIONS;
			MPI_Bcast(&method, 1, MPI_INT, 0, phreeqcrm_comm);
		}
		// convert Reaction module solution data to concentrations for transport
		std::vector<double> d;  // scratch space to convert from moles to mass fraction
		std::vector<double> solns;
		cxxNameDouble::iterator it;

		// Put solutions into a vector
		int n = this->mpi_myself;
		for (int j = this->start_cell[n]; j <= this->end_cell[n]; j++)
		{
			// load fractions into d
			cxxSolution * cxxsoln_ptr = this->GetWorkers()[0]->Get_solution(j);
			assert (cxxsoln_ptr);
			double v, dens;
			if (this->use_solution_density_volume)
			{
				v = cxxsoln_ptr->Get_soln_vol();
				dens = cxxsoln_ptr->Get_density();
			}
			else
			{
				//int k = this->backward_mapping[j][0];
				int l = j - this->start_cell[n];
				v = this->saturation_worker[l] * this->porosity_worker[l] * this->rv_worker[l];

				if (v <= 0)
				{
					v = cxxsoln_ptr->Get_soln_vol();
				}
				dens = this->density_worker[j - this->start_cell[n]];
			}
			this->cxxSolution2concentration(cxxsoln_ptr, d, v, dens);
			for (int i = 0; i < (int) this->components.size(); i++)
			{
				solns.push_back(d[i]);
			}
		}

		// make buffer to recv solutions
		double * recv_solns = NULL;
		int * recv_counts = NULL;
		int * recv_displs = NULL;
		if (this->mpi_myself == 0)
		{
			recv_solns = new double[(size_t) this->count_chemistry * this->components.size()];
			recv_counts = new int[this->mpi_tasks];
			recv_displs = new int[this->mpi_tasks];
			for (int i = 0; i < this->mpi_tasks; i++)
			{
				recv_counts[i] = (end_cell[i] - start_cell[i] + 1) * (int) this->components.size();
				recv_displs[i] = start_cell[i] * (int) this->components.size();
			}
		}

		// Gather to root
		double * buf = &solns[0];
		int my_length = (end_cell[this->mpi_myself] - start_cell[this->mpi_myself] + 1) * (int) this->components.size();
		MPI_Gatherv(buf, my_length, MPI_DOUBLE,
			recv_solns, recv_counts, recv_displs, MPI_DOUBLE, 0, phreeqcrm_comm);

		// Root processes to c
		if (mpi_myself == 0)
		{
			// check size and fill elements, if necessary resize
			c.resize(this->nxyz * this->components.size());
			std::fill(c.begin(), c.end(), INACTIVE_CELL_VALUE);

			// Write vector into c
			int n = 0;
			for (int j = 0; j < count_chemistry; j++)
			{
				std::vector<double> d;
				for (size_t i = 0; i < this->components.size(); i++)
				{
					d.push_back(recv_solns[n++]);
				}
				std::vector<int>::iterator it;
				for (it = this->backward_mapping[j].begin(); it != this->backward_mapping[j].end(); it++)
				{
					double *d_ptr = &c[*it];
					size_t i;
					for (i = 0; i < this->components.size(); i++)
					{
						d_ptr[this->nxyz * i] = d[i];
					}
				}
			}
			delete [] recv_solns;
			delete [] recv_counts;
			delete [] recv_displs;
		}
	}
	catch (...)
	{
		return_value = IRM_FAIL;
	}
	return this->ReturnHandler(return_value, "PhreeqcRM::GetConcentrations");
}
#else
/* ---------------------------------------------------------------------- */
IRM_RESULT
PhreeqcRM::GetConcentrations(std::vector<double> &c)
/* ---------------------------------------------------------------------- */
{
	this->phreeqcrm_error_string.clear();
	// convert Reaction module solution data to hst mass fractions
	IRM_RESULT return_value = IRM_OK;
	try
	{
		// check size and fill elements, if necessary resize
		c.resize(this->nxyz * this->components.size());
		std::fill(c.begin(), c.end(), INACTIVE_CELL_VALUE);

		std::vector<double> d;  // scratch space to convert from moles to mass fraction
		cxxSolution * cxxsoln_ptr;
		for (int n = 0; n < this->nthreads; n++)
		{
			for (int j = this->start_cell[n]; j <= this->end_cell[n]; j++)
			{
				// load fractions into d
				cxxsoln_ptr = this->GetWorkers()[n]->Get_solution(j);
				assert (cxxsoln_ptr);
				double v, dens;
				if (this->use_solution_density_volume)
				{
					v = cxxsoln_ptr->Get_soln_vol();
					dens = cxxsoln_ptr->Get_density();
				}
				else
				{
					int k = this->backward_mapping[j][0];
					v =  this->saturation_root[k]  * this->porosity_root[k] * this->rv_root[k];
					dens = this->density_root[k];
					if (v <= 0)
					{
						v = cxxsoln_ptr->Get_soln_vol();
					}
				}
				this->cxxSolution2concentration(cxxsoln_ptr, d, v, dens);

				// store in fraction at 1, 2, or 4 places depending on chemistry dimensions
				std::vector<int>::iterator it;
				for (it = this->backward_mapping[j].begin(); it != this->backward_mapping[j].end(); it++)
				{
					double *d_ptr = &c[*it];
					size_t i;
					for (i = 0; i < this->components.size(); i++)
					{
						d_ptr[this->nxyz * i] = d[i];
					}
				}
			}
		}
	}
	catch (...)
	{
		return_value = IRM_FAIL;
	}
	return this->ReturnHandler(return_value, "PhreeqcRM::GetConcentrations");
}
#endif
/* ---------------------------------------------------------------------- */
int
PhreeqcRM::GetCurrentSelectedOutputUserNumber(void)
/* ---------------------------------------------------------------------- */
{
	this->phreeqcrm_error_string.clear();
	try
	{
		return this->workers[0]->GetCurrentSelectedOutputUserNumber();
	}
	catch (...)
	{
	}
	return this->ReturnHandler(IRM_INVALIDARG, "PhreeqcRM::GetCurrentSelectedOutputUserNumber");
}
/* ---------------------------------------------------------------------- */
IRM_RESULT
PhreeqcRM::GetDensity(std::vector<double>& density_arg)
/* ---------------------------------------------------------------------- */
{
	// For backward compatibility
	return GetDensityCalculated(density_arg);
}
/* ---------------------------------------------------------------------- */
IRM_RESULT
PhreeqcRM::GetDensityCalculated(std::vector<double> & density_arg)
/* ---------------------------------------------------------------------- */
{
	this->phreeqcrm_error_string.clear();
	try
	{
#ifdef USE_MPI
		if (this->mpi_myself == 0)
		{
			int method = METHOD_GETDENSITYCALCULATED;
			MPI_Bcast(&method, 1, MPI_INT, 0, phreeqcrm_comm);
		}
		std::vector<double> local_density_worker;
		int size = this->end_cell[this->mpi_myself] - this->start_cell[this->mpi_myself] + 1;
		local_density_worker.resize(size, INACTIVE_CELL_VALUE);
		
		// fill saturation_root
		int n = this->mpi_myself;
		for (int i = this->start_cell[n]; i <= this->end_cell[n]; i++)
		{
			int l = i - this->start_cell[n];
			local_density_worker[l] = this->workers[0]->Get_solution(i)->Get_density();
		}

		// Gather to root
		GatherNchem(local_density_worker, density_arg);
#else
		density_arg.resize(this->nxyz, INACTIVE_CELL_VALUE);
		std::vector<double> dbuffer;
		for (int n = 0; n < this->nthreads; n++)
		{
			for (int i = start_cell[n]; i <= this->end_cell[n]; i++)
			{
				cxxSolution * soln_ptr = this->workers[n]->Get_solution(i);
				if (!soln_ptr)
				{
					std::ostringstream oss;
					oss << "Solution not found for density." << "  thread: " << n << "  solution " << i;
					this->ErrorHandler(IRM_FAIL, oss.str());
				}
				else
				{
					double d = this->workers[n]->Get_solution(i)->Get_density();
					for(size_t j = 0; j < backward_mapping[i].size(); j++)
					{
						int n = backward_mapping[i][j];
						density_arg[n] = d;
					}
				}
			}
		}
#endif
	}
	catch (...)
	{
		this->ReturnHandler(IRM_FAIL, "PhreeqcRM::GetDensityCalculated");
	}
	return IRM_OK;
}

/* ---------------------------------------------------------------------- */
std::string
PhreeqcRM::GetErrorString(void)
/* ---------------------------------------------------------------------- */
{
	std::string cummulative_error_string;
	if (this->mpi_myself == 0)
	{
		cummulative_error_string.append(this->phreeqcrm_error_string);
	}
	try
	{
#ifdef USE_MPI
		if (this->mpi_myself == 0)
		{
			int method = METHOD_GETERRORSTRING;
			MPI_Bcast(&method, 1, MPI_INT, 0, phreeqcrm_comm);
		}
		for (int n = 1; n < this->mpi_tasks; n++)
		{
			if (this->mpi_myself == n)
			{
					int l = (int) this->phreeqcrm_error_string.size();
					MPI_Send((void *) &l, 1, MPI_INT, 0, 0, phreeqcrm_comm);
					if (l > 0)
					{
						MPI_Send((void *) this->phreeqcrm_error_string.c_str(), l, MPI_CHAR, 0, 0, phreeqcrm_comm);
					}
			}
			else if (this->mpi_myself == 0)
			{
				std::string errstr;
				MPI_Status mpi_status;
				int l;
				MPI_Recv(&l, 1, MPI_INT, n, 0, phreeqcrm_comm, &mpi_status);
				if (l > 0)
				{
					char *errstr = new char[l + 1];
					MPI_Recv(errstr, l, MPI_CHAR, n, 0, phreeqcrm_comm, &mpi_status);
					cummulative_error_string.append(errstr);
					delete []errstr;
				}
			}
		}
#endif
	}
	catch (...)
	{
		this->ReturnHandler(IRM_FAIL, "PhreeqcRM::GetGetErrorString");
	}
	return cummulative_error_string;
}

#ifdef USE_MPI
/* ---------------------------------------------------------------------- */
IRM_RESULT
PhreeqcRM::GetGasCompMoles(std::vector<double>& m_out)
/* ---------------------------------------------------------------------- */
{
	// retrieve gas component moles
	this->phreeqcrm_error_string.clear();
	IRM_RESULT return_value = IRM_OK;
	try
	{
		if (this->mpi_myself == 0)
		{
			int method = METHOD_GETGASCOMPMOLES;
			MPI_Bcast(&method, 1, MPI_INT, 0, phreeqcrm_comm);
		}
		const std::vector<std::string>& gc_names = GetGasComponents();
		int n = this->mpi_myself;
		std::vector<double> send_gas_moles;
		for (int j = this->start_cell[n]; j <= this->end_cell[n]; j++)
		{
			cxxGasPhase* gas_ptr = this->GetWorkers()[0]->Get_gas_phase(j);
			for (size_t k = 0; k < gc_names.size(); k++)
			{
				double moles = (gas_ptr != NULL) ? gas_ptr->Get_component_moles(gc_names[k]) : -1.0;
				send_gas_moles.push_back(moles);
			}
		}

		// make buffer to recv solutions
		double* recv_gas_moles = NULL;
		int* recv_counts = NULL;
		int* recv_displs = NULL;
		if (this->mpi_myself == 0)
		{
			recv_gas_moles = new double[(size_t)this->count_chemistry * gc_names.size()];
			recv_counts = new int[this->mpi_tasks];
			recv_displs = new int[this->mpi_tasks];
			for (int i = 0; i < this->mpi_tasks; i++)
			{
				recv_counts[i] = (end_cell[i] - start_cell[i] + 1) * (int) gc_names.size();
				recv_displs[i] = start_cell[i] * (int) gc_names.size();
			}
		}

		// Gather to root
		double* send_buf = &send_gas_moles[0];
		int my_length = (end_cell[this->mpi_myself] - start_cell[this->mpi_myself] + 1) * (int)gc_names.size();
		MPI_Gatherv(send_buf, my_length, MPI_DOUBLE,
			recv_gas_moles, recv_counts, recv_displs, MPI_DOUBLE, 0, phreeqcrm_comm);

		// Root processes to m_out
		if (mpi_myself == 0)
		{
			// check size and fill elements, if necessary resize
			m_out.resize(this->nxyz * gc_names.size());
			std::fill(m_out.begin(), m_out.end(), 1e30);

			int n = 0;
			for (int j = 0; j < count_chemistry; j++)
			{
				std::vector<double> d;
				for (size_t i = 0; i < gc_names.size(); i++)
				{
					d.push_back(recv_gas_moles[n++]);
				}
				std::vector<int>::iterator it;
				for (it = this->backward_mapping[j].begin(); it != this->backward_mapping[j].end(); it++)
				{
					double* m_ptr = &m_out[*it];
					size_t i;
					for (i = 0; i < gc_names.size(); i++)
					{
						m_ptr[this->nxyz * i] = d[i];
					}
				}
			}
			delete[] recv_gas_moles;
			delete[] recv_counts;
			delete[] recv_displs;
		}
	}
	catch (...)
	{
		return_value = IRM_FAIL;
	}
	return this->ReturnHandler(return_value, "PhreeqcRM::GetGasCompMoles");
}
#else
/* ---------------------------------------------------------------------- */
IRM_RESULT
PhreeqcRM::GetGasCompMoles(std::vector<double>& m_out)
/* ---------------------------------------------------------------------- */
{
	// retrieve gas component moles
	this->phreeqcrm_error_string.clear();
	IRM_RESULT return_value = IRM_OK;
	try
	{
		// resize and fill elements
		m_out.resize(this->nxyz * this->GetGasComponentsCount());
		std::fill(m_out.begin(), m_out.end(), 1e30);
#ifdef USE_OPENMP
		omp_set_num_threads(this->nthreads);
#pragma omp parallel
#pragma omp for
#endif
		for (int n = 0; n < this->nthreads; n++)
		{
			const std::vector<std::string>& gc_names = GetGasComponents();
			for (int j = this->start_cell[n]; j <= this->end_cell[n]; j++)
			{
				std::vector<double> d;
				cxxGasPhase* gas_ptr = this->GetWorkers()[n]->Get_gas_phase(j);
				for (size_t k = 0; k < gc_names.size(); k++)
				{
					double moles = (gas_ptr != NULL) ? gas_ptr->Get_component_moles(gc_names[k]) : -1.0;
					d.push_back(moles);
				}

				// store in fraction at 1, 2, or 4 places depending on chemistry dimensions
				std::vector<int>::iterator it;
				for (it = this->backward_mapping[j].begin(); it != this->backward_mapping[j].end(); it++)
				{
					double* m_ptr = &m_out[*it];
					size_t i;
					for (i = 0; i < gc_names.size(); i++)
					{
						m_ptr[this->nxyz * i] = d[i];
					}
				}
			}
		}
	}
	catch (...)
	{
		return_value = IRM_FAIL;
	}
	return this->ReturnHandler(return_value, "PhreeqcRM::GetGasCompMoles");
}
#endif

#ifdef USE_MPI
/* ---------------------------------------------------------------------- */
IRM_RESULT
PhreeqcRM::GetGasCompPressures(std::vector<double>& p_out)
/* ---------------------------------------------------------------------- */
{
	// retrieve pressures
	this->phreeqcrm_error_string.clear();
	IRM_RESULT return_value = IRM_OK;
	try
	{
		if (this->mpi_myself == 0)
		{
			int method = METHOD_GETGASCOMPPRESSURES;
			MPI_Bcast(&method, 1, MPI_INT, 0, phreeqcrm_comm);
		}
		// retrieve reaction module pressures of gas components 
		const std::vector<std::string>& gc_names = GetGasComponents();

		int n = this->mpi_myself;
		std::vector<double> send_gas_p;
		for (int j = this->start_cell[n]; j <= this->end_cell[n]; j++)
		{
			cxxGasPhase* gas_ptr = this->GetWorkers()[0]->Get_gas_phase(j);
			for (size_t k = 0; k < gc_names.size(); k++)
			{
				double pressure = (gas_ptr != NULL) ? gas_ptr->Get_component_p(gc_names[k]) : -1.0;
				send_gas_p.push_back(pressure);
			}
		}

		double* recv_gas_p = NULL;
		int* recv_counts = NULL;
		int* recv_displs = NULL;
		if (this->mpi_myself == 0)
		{
			recv_gas_p = new double[(size_t)this->count_chemistry * gc_names.size()];
			recv_counts = new int[this->mpi_tasks];
			recv_displs = new int[this->mpi_tasks];
			for (int i = 0; i < this->mpi_tasks; i++)
			{
				recv_counts[i] = (end_cell[i] - start_cell[i] + 1) * (int)gc_names.size();
				recv_displs[i] = start_cell[i] * (int)gc_names.size();
			}
		}

		// Gather to root
		double* send_buf = &send_gas_p[0];
		int my_length = (end_cell[this->mpi_myself] - start_cell[this->mpi_myself] + 1) * (int)gc_names.size();
		MPI_Gatherv(send_buf, my_length, MPI_DOUBLE,
			recv_gas_p, recv_counts, recv_displs, MPI_DOUBLE, 0, phreeqcrm_comm);

		// Root processes to m
		if (mpi_myself == 0)
		{
			// check size and fill elements, if necessary resize
			p_out.resize(this->nxyz * gc_names.size());
			std::fill(p_out.begin(), p_out.end(), 1e30);

			int n = 0;
			for (int j = 0; j < count_chemistry; j++)
			{
				std::vector<double> d;
				for (size_t i = 0; i < gc_names.size(); i++)
				{
					d.push_back(recv_gas_p[n++]);
				}
				std::vector<int>::iterator it;
				for (it = this->backward_mapping[j].begin(); it != this->backward_mapping[j].end(); it++)
				{
					double* p_ptr = &p_out[*it];
					size_t i;
					for (i = 0; i < gc_names.size(); i++)
					{
						p_ptr[this->nxyz * i] = d[i];
					}
				}
			}
			delete[] recv_gas_p;
			delete[] recv_counts;
			delete[] recv_displs;
		}
	}
	catch (...)
	{
		return_value = IRM_FAIL;
	}
	return this->ReturnHandler(return_value, "PhreeqcRM::GetGasCompPressures");
}
#else
/* ---------------------------------------------------------------------- */
IRM_RESULT
PhreeqcRM::GetGasCompPressures(std::vector<double>& p_out)
/* ---------------------------------------------------------------------- */
{
	// retrieve pressures
	this->phreeqcrm_error_string.clear();
	IRM_RESULT return_value = IRM_OK;
	try
	{
		// resize and fill elements
		p_out.resize(this->nxyz * this->GetGasComponentsCount());
		std::fill(p_out.begin(), p_out.end(), 1e30);
#ifdef USE_OPENMP
		omp_set_num_threads(this->nthreads);
#pragma omp parallel
#pragma omp for
#endif
		for (int n = 0; n < this->nthreads; n++)
		{
			const std::vector<std::string>& gc_names = GetGasComponents();
			for (int j = this->start_cell[n]; j <= this->end_cell[n]; j++)
			{
				cxxGasPhase* gas_ptr = this->GetWorkers()[n]->Get_gas_phase(j);
				std::vector<double> d;
				for (size_t k = 0; k < gc_names.size(); k++)
				{
					double pressure = (gas_ptr != NULL) ? gas_ptr->Get_component_p(gc_names[k]) : -1.0;
					d.push_back(pressure);
				}
				// store in fraction at 1, 2, or 4 places depending on chemistry dimensions
				std::vector<int>::iterator it;
				for (it = this->backward_mapping[j].begin(); it != this->backward_mapping[j].end(); it++)
				{
					double* p_ptr = &p_out[*it];
					for (size_t i = 0; i < gc_names.size(); i++)
					{
						p_ptr[this->nxyz * i] = d[i];
					}
				}
			}
		}
	}
	catch (...)
	{
		return_value = IRM_FAIL;
	}
	return this->ReturnHandler(return_value, "PhreeqcRM::GetGasCompPressures");
}
#endif

#ifdef USE_MPI
/* ---------------------------------------------------------------------- */
IRM_RESULT
PhreeqcRM::GetGasCompPhi(std::vector<double>& phi_out)
/* ---------------------------------------------------------------------- */
{
	// retrieve phi
	this->phreeqcrm_error_string.clear();
	IRM_RESULT return_value = IRM_OK;
	try
	{
		if (this->mpi_myself == 0)
		{
			int method = METHOD_GETGASCOMPPHI;
			MPI_Bcast(&method, 1, MPI_INT, 0, phreeqcrm_comm);
		}
		const std::vector<std::string>& gc_names = GetGasComponents();
		int n = this->mpi_myself;
		std::vector<double> send_gas_phi;
		for (int j = this->start_cell[n]; j <= this->end_cell[n]; j++)
		{
			cxxGasPhase* gas_ptr = this->GetWorkers()[0]->Get_gas_phase(j);
			for (size_t k = 0; k < gc_names.size(); k++)
			{
				double phi = (gas_ptr != NULL) ? gas_ptr->Get_component_phi(gc_names[k]) : -1.0;
				send_gas_phi.push_back(phi);
			}
		}

		// make buffer to recv solutions
		double* recv_gas_phis = NULL;
		int* recv_counts = NULL;
		int* recv_displs = NULL;
		if (this->mpi_myself == 0)
		{
			recv_gas_phis = new double[(size_t)this->count_chemistry * gc_names.size()];
			recv_counts = new int[this->mpi_tasks];
			recv_displs = new int[this->mpi_tasks];
			for (int i = 0; i < this->mpi_tasks; i++)
			{
				recv_counts[i] = (end_cell[i] - start_cell[i] + 1) * (int)gc_names.size();
				recv_displs[i] = start_cell[i] * (int)gc_names.size();
			}
		}

		// Gather to root
		double* buf = &send_gas_phi[0];
		int my_length = (end_cell[this->mpi_myself] - start_cell[this->mpi_myself] + 1) * (int)gc_names.size();
		MPI_Gatherv(buf, my_length, MPI_DOUBLE,
			recv_gas_phis, recv_counts, recv_displs, MPI_DOUBLE, 0, phreeqcrm_comm);

		// Root processes to m
		if (mpi_myself == 0)
		{
			// check size and fill elements, if necessary resize
			phi_out.resize(this->nxyz * gc_names.size());
			std::fill(phi_out.begin(), phi_out.end(), 1e30);

			// Write vector into m
			int n = 0;
			for (int j = 0; j < count_chemistry; j++)
			{
				std::vector<double> d;
				for (size_t i = 0; i < gc_names.size(); i++)
				{
					d.push_back(recv_gas_phis[n++]);
				}
				std::vector<int>::iterator it;
				for (it = this->backward_mapping[j].begin(); it != this->backward_mapping[j].end(); it++)
				{
					double* phi_ptr = &phi_out[*it];
					size_t i;
					for (i = 0; i < gc_names.size(); i++)
					{
						phi_ptr[this->nxyz * i] = d[i];
					}
				}
			}
			delete[] recv_gas_phis;
			delete[] recv_counts;
			delete[] recv_displs;
		}
	}
	catch (...)
	{
		return_value = IRM_FAIL;
	}
	return this->ReturnHandler(return_value, "PhreeqcRM::GetGasCompPhi");
}
#else
/* ---------------------------------------------------------------------- */
IRM_RESULT
PhreeqcRM::GetGasCompPhi(std::vector<double>& phi_out)
/* ---------------------------------------------------------------------- */
{
	// retrieve phi
	this->phreeqcrm_error_string.clear();
	IRM_RESULT return_value = IRM_OK;
	try
	{
		// resize and fill elements
		phi_out.resize(this->nxyz * this->GetGasComponentsCount());
		std::fill(phi_out.begin(), phi_out.end(), 1e30);
#ifdef USE_OPENMP
		omp_set_num_threads(this->nthreads);
#pragma omp parallel
#pragma omp for
#endif
		for (int n = 0; n < this->nthreads; n++)
		{
			const std::vector<std::string>& gc_names = GetGasComponents();
			for (int j = this->start_cell[n]; j <= this->end_cell[n]; j++)
			{
				cxxGasPhase * gas_ptr = this->GetWorkers()[n]->Get_gas_phase(j);
				std::vector<double> d;
				for (size_t k = 0; k < gc_names.size(); k++)
				{
					double phi = (gas_ptr != NULL) ? gas_ptr->Get_component_phi(gc_names[k]) : -1.0;
					d.push_back(phi);
				}
				// store in fraction at 1, 2, or 4 places depending on chemistry dimensions
				std::vector<int>::iterator it;
				for (it = this->backward_mapping[j].begin(); it != this->backward_mapping[j].end(); it++)
				{
					double* phi_ptr = &phi_out[*it];
					for (size_t i = 0; i < gc_names.size(); i++)
					{
						phi_ptr[this->nxyz * i] = d[i];
					}
				}
			}
		}
	}
	catch (...)
	{
		return_value = IRM_FAIL;
	}
	return this->ReturnHandler(return_value, "PhreeqcRM::GetGasCompPhi");
}
#endif


#ifdef USE_MPI
/* ---------------------------------------------------------------------- */
IRM_RESULT
PhreeqcRM::GetGasPhaseVolume(std::vector<double>& v_out)
/* ---------------------------------------------------------------------- */
{
	// retrieve gas phase volumes
	this->phreeqcrm_error_string.clear();
	IRM_RESULT return_value = IRM_OK;
	try
	{
		if (this->mpi_myself == 0)
		{
			int method = METHOD_GETGASPHASEVOLUME;
			MPI_Bcast(&method, 1, MPI_INT, 0, phreeqcrm_comm);
		}
		const std::vector<std::string>& gc_names = GetGasComponents();
		int n = this->mpi_myself;
		std::vector<double> send_gas_v;
		for (int j = this->start_cell[n]; j <= this->end_cell[n]; j++)
		{
			cxxGasPhase* gas_ptr = this->GetWorkers()[0]->Get_gas_phase(j);
			double volume = (gas_ptr != NULL) ? gas_ptr->Get_volume() : -1.0;
			send_gas_v.push_back(volume);
		}

		// make buffer to recv solutions
		double* recv_gas_p = NULL;
		int* recv_counts = NULL;
		int* recv_displs = NULL;
		if (this->mpi_myself == 0)
		{
			recv_gas_p = new double[(size_t)this->count_chemistry];
			recv_counts = new int[this->mpi_tasks];
			recv_displs = new int[this->mpi_tasks];
			for (int i = 0; i < this->mpi_tasks; i++)
			{
				recv_counts[i] = (end_cell[i] - start_cell[i] + 1);
				recv_displs[i] = start_cell[i];
			}
		}

		// Gather to root
		double* send_buf = &send_gas_v[0];
		int my_length = (end_cell[this->mpi_myself] - start_cell[this->mpi_myself] + 1);
		MPI_Gatherv(send_buf, my_length, MPI_DOUBLE,
			recv_gas_p, recv_counts, recv_displs, MPI_DOUBLE, 0, phreeqcrm_comm);

		if (mpi_myself == 0)
		{
			// resize and fill elements
			v_out.resize(this->nxyz);
			std::fill(v_out.begin(), v_out.end(), 1e30);
			for (int j = 0; j < count_chemistry; j++)
			{
				std::vector<int>::iterator it;
				for (it = this->backward_mapping[j].begin(); it != this->backward_mapping[j].end(); it++)
				{
					v_out[*it] = recv_gas_p[j];
				}
			}
			delete[] recv_gas_p;
			delete[] recv_counts;
			delete[] recv_displs;
		}
	}
	catch (...)
	{
		return_value = IRM_FAIL;
	}
	return this->ReturnHandler(return_value, "PhreeqcRM::GetGasPhaseVolume");
}
#else
/* ---------------------------------------------------------------------- */
IRM_RESULT
PhreeqcRM::GetGasPhaseVolume(std::vector<double>& v_out)
/* ---------------------------------------------------------------------- */
{
	// retrieve pressures
	this->phreeqcrm_error_string.clear();
	IRM_RESULT return_value = IRM_OK;
	try
	{
		// resize and fill elements
		v_out.resize(this->nxyz);
		std::fill(v_out.begin(), v_out.end(), 1e30);
#ifdef USE_OPENMP
		omp_set_num_threads(this->nthreads);
#pragma omp parallel
#pragma omp for
#endif
		for (int n = 0; n < this->nthreads; n++)
		{
			for (int j = this->start_cell[n]; j <= this->end_cell[n]; j++)
			{
				cxxGasPhase * gas_ptr = this->GetWorkers()[n]->Get_gas_phase(j);
				double volume;
				volume = (gas_ptr != NULL) ? gas_ptr->Get_volume() : -1.0;
				std::vector<int>::iterator it;
				for (it = this->backward_mapping[j].begin(); it != this->backward_mapping[j].end(); it++)
				{
					v_out[*it] = volume;
				}
			}
		}
	}
	catch (...)
	{
		return_value = IRM_FAIL;
	}
	return this->ReturnHandler(return_value, "PhreeqcRM::GetGasPhaseVolume");
}
#endif

/* ---------------------------------------------------------------------- */
IPhreeqc *
PhreeqcRM::GetIPhreeqcPointer(int i)
/* ---------------------------------------------------------------------- */
{
	return (i >= 0 && i < this->nthreads + 2) ? this->workers[i] : NULL;
}
/* ---------------------------------------------------------------------- */
int
PhreeqcRM::GetNthSelectedOutputUserNumber(int i)
/* ---------------------------------------------------------------------- */
{
	this->phreeqcrm_error_string.clear();
	int return_value = IRM_OK;
	try
	{
		if (i >= 0)
		{
			return_value = this->workers[0]->GetNthSelectedOutputUserNumber(i);
			this->ErrorHandler(return_value, "GetNthSelectedOutputUserNumber");
		}
		else
		{
			this->ErrorHandler(IRM_INVALIDARG, "GetNthSelectedOutputUserNumber");
		}
	}
	catch (...)
	{
		return_value = IRM_FAIL;
	}
	this->ReturnHandler(PhreeqcRM::Int2IrmResult(return_value, true), "PhreeqcRM::GetNthSelectedOutputUserNumber");
	return return_value;
}

/* ---------------------------------------------------------------------- */
const std::vector<double>&
PhreeqcRM::GetPorosity(void)
/* ---------------------------------------------------------------------- */
{
	return this->porosity_root;
}
#ifdef USE_MPI
/* ---------------------------------------------------------------------- */
const std::vector<double> &
PhreeqcRM::GetPressure(void)
/* ---------------------------------------------------------------------- */
{
	this->phreeqcrm_error_string.clear();
	try
	{
		if (this->mpi_myself == 0)
		{
			int method = METHOD_GETPRESSURE;
			MPI_Bcast(&method, 1, MPI_INT, 0, phreeqcrm_comm);
		}
		int size = this->end_cell[this->mpi_myself] - this->start_cell[this->mpi_myself] + 1;
		this->pressure_worker.resize(size, INACTIVE_CELL_VALUE);
		
		// fill pressure
		int n = this->mpi_myself;
		for (int i = this->start_cell[n]; i <= this->end_cell[n]; i++)
		{
			this->pressure_worker[i - this->start_cell[n]] = this->workers[0]->Get_solution(i)->Get_patm();
		}
		
		// Gather to root
		GatherNchem(this->pressure_worker, this->pressure_root);

	}
	catch (...)
	{
		this->ReturnHandler(IRM_FAIL, "PhreeqcRM::GetPressure");
		this->pressure_root.clear();
	}
	return this->pressure_root;
}
#else
/* ---------------------------------------------------------------------- */
const std::vector<double> &
PhreeqcRM::GetPressure(void)
/* ---------------------------------------------------------------------- */
{
	this->phreeqcrm_error_string.clear();
	try
	{

		this->pressure_root.resize(this->nxyz, INACTIVE_CELL_VALUE);
		std::vector<double> dbuffer;

		for (int n = 0; n < this->nthreads; n++)
		{
			for (int i = start_cell[n]; i <= this->end_cell[n]; i++)
			{
				if (this->workers[n]->Get_solution(i))
				{
					double d = this->workers[n]->Get_solution(i)->Get_patm();
					for(size_t j = 0; j < backward_mapping[i].size(); j++)
					{
						int n = backward_mapping[i][j];
						this->pressure_root[n] = d;
					}
				}
				else
				{
					std::ostringstream e_stream;
				    e_stream << "Solution not found in GetPressure " << i << std::endl;
					this->ErrorMessage(e_stream.str());
				}
			}
		}
	}
	catch (...)
	{
		this->ReturnHandler(IRM_FAIL, "PhreeqcRM::GetPressure");
		this->pressure_root.clear();
	}
	return this->pressure_root;
}
#endif

/* ---------------------------------------------------------------------- */
IRM_RESULT
PhreeqcRM::GetSaturation(std::vector<double>& sat_arg)
/* ---------------------------------------------------------------------- */
{
	// For backward compatibility
	return GetSaturationCalculated(sat_arg);
}
/* ---------------------------------------------------------------------- */
IRM_RESULT
PhreeqcRM::GetSaturationCalculated(std::vector<double> & sat_arg)
/* ---------------------------------------------------------------------- */
{
	this->phreeqcrm_error_string.clear();
	try
	{
#ifdef USE_MPI
		if (this->mpi_myself == 0)
		{
			int method = METHOD_GETSATURATIONCALCULATED;
			MPI_Bcast(&method, 1, MPI_INT, 0, phreeqcrm_comm);
		}
		std::vector<double> local_saturation_worker;
		int size = this->end_cell[this->mpi_myself] - this->start_cell[this->mpi_myself] + 1;
		local_saturation_worker.resize(size, INACTIVE_CELL_VALUE);
		
		// fill saturation_root
		int n = this->mpi_myself;
		for (int i = this->start_cell[n]; i <= this->end_cell[n]; i++)
		{
			double v = this->workers[0]->Get_solution(i)->Get_soln_vol();
			int l = i - this->start_cell[n];
			local_saturation_worker[l] = v / (this->rv_worker[l] * this->porosity_worker[l]);
		}

		// Gather to root
		GatherNchem(local_saturation_worker, sat_arg);
#else
		sat_arg.resize(this->nxyz, INACTIVE_CELL_VALUE);
		std::vector<double> dbuffer;
		for (int n = 0; n < this->nthreads; n++)
		{
			for (int i = start_cell[n]; i <= this->end_cell[n]; i++)
			{
				cxxSolution * soln_ptr = this->workers[n]->Get_solution(i);
				if (!soln_ptr)
				{
					this->ErrorHandler(IRM_FAIL, "Solution not found for saturation.");
				}
				else
				{
					double v = soln_ptr->Get_soln_vol();
					for(size_t j = 0; j < backward_mapping[i].size(); j++)
					{
						int n = backward_mapping[i][j];
						sat_arg[n] = v / (this->rv_root[n] * this->porosity_root[n]);
					}
				}
			}
		}
#endif
	}
	catch (...)
	{
		this->ReturnHandler(IRM_FAIL, "PhreeqcRM::GetSaturationCalculated");
		sat_arg.clear();
	}
	return IRM_OK;
}
/* ---------------------------------------------------------------------- */
IRM_RESULT
PhreeqcRM::GetSelectedOutput(std::vector<double> &so)
/* ---------------------------------------------------------------------- */
{
	this->phreeqcrm_error_string.clear();
	IRM_RESULT return_value = IRM_OK;
	try
	{
#ifdef USE_MPI
		if (this->mpi_myself == 0)
		{
			int method = METHOD_GETSELECTEDOUTPUT;
			MPI_Bcast(&method, 1, MPI_INT, 0, phreeqcrm_comm);
		}
#endif
		int n_user = this->workers[0]->GetCurrentSelectedOutputUserNumber();
#ifdef USE_MPI
		MPI_Bcast(&n_user,  1, MPI_INT, 0, phreeqcrm_comm);
		if (n_user < 0)
		{
			this->ErrorHandler(IRM_INVALIDARG, "No selected output defined");
		}
		std::vector<int> r_values;
		r_values.resize(1,0);
		try
		{
			std::map< int, CSelectedOutput >::iterator it = this->workers[0]->CSelectedOutputMap.find(n_user);
			if (it == this->workers[0]->CSelectedOutputMap.end())
				this->ErrorHandler(IRM_INVALIDARG, "Selected output not found");
			if (this->SetCurrentSelectedOutputUserNumber(n_user) < 0)
				this->ErrorHandler(IRM_INVALIDARG, "Selected output not found");;
			int ncol = this->GetSelectedOutputColumnCount();
			int local_start_cell = 0;
			std::vector<double> dbuffer;

			// fill with INACTIVE_CELL_VALUE
			if (mpi_myself == 0)
			{
				so.resize(this->nxyz * ncol);
			}
			for (int n = 0; n < this->mpi_tasks; n++)
			{
				int nrow;
				if (this->mpi_myself == n)
				{
					if (this->mpi_myself == 0)
					{
						it->second.Doublize(nrow, ncol, dbuffer);
					}
					else
					{
						it->second.Doublize(nrow, ncol, dbuffer);
						int length[2];
						length[0] = nrow;
						length[1] = ncol;
						MPI_Send(length, 2, MPI_INT, 0, 0, phreeqcrm_comm);
						MPI_Send(&dbuffer.front(), nrow*ncol, MPI_DOUBLE, 0, 0, phreeqcrm_comm);
					}

				}
				else if (this->mpi_myself == 0)
				{
					MPI_Status mpi_status;
					int length[2];
					MPI_Recv(length, 2, MPI_INT, n, 0, phreeqcrm_comm, &mpi_status);
					nrow = length[0];
					ncol = length[1];
					dbuffer.resize(nrow*ncol);
					MPI_Recv(&dbuffer.front(), nrow*ncol, MPI_DOUBLE, n, 0, phreeqcrm_comm, &mpi_status);
				}
				if (mpi_myself == 0)
				{
					// Now write data from the process to so
					for (int icol = 0; icol < ncol; icol++)
					{
						for (int irow = 0; irow < nrow; irow++)
						{
							int ichem = local_start_cell + (int) irow;
							for (size_t k = 0; k < backward_mapping[ichem].size(); k++)
							{
								int ixyz = backward_mapping[ichem][k];
								so[icol*this->nxyz + ixyz] = dbuffer[icol*nrow + irow];
							}
						}
					}
					local_start_cell += nrow;
				}
			}
		}
		catch (...)
		{
			r_values[0] = 1;
		}
		this->HandleErrorsInternal(r_values);
#else
		if (n_user < 0)
			this->ErrorHandler(IRM_INVALIDARG, "Selected output not defined.");
		if (this->SetCurrentSelectedOutputUserNumber(n_user) < 0)
			this->ErrorHandler(IRM_INVALIDARG, "Selected output not found.");
		int ncol = this->GetSelectedOutputColumnCount();
		std::vector<double> dbuffer;
		int local_start_cell = 0;

		// resize target
		so.resize(this->nxyz * ncol);
		for (int n = 0; n < this->nthreads; n++)
		{
			int nrow_x=-1, ncol_x=-1;
			std::map< int, CSelectedOutput>::iterator cso_it = this->workers[n]->CSelectedOutputMap.find(n_user);
			if (cso_it == this->workers[n]->CSelectedOutputMap.end())
			{
				this->ErrorHandler(IRM_INVALIDARG, "Did not find current selected output in CSelectedOutputMap");
			}
			else
			{
				cso_it->second.Doublize(nrow_x, ncol_x, dbuffer);
				assert(ncol_x = ncol);

				// Now write data from thread to so
				for (size_t icol = 0; icol < (size_t) ncol; icol++)
				{
					for (size_t irow = 0; irow < (size_t) nrow_x; irow++)
					{
						int ichem = local_start_cell + (int) irow;
						for (size_t k = 0; k < backward_mapping[ichem].size(); k++)
						{
							int ixyz = backward_mapping[ichem][k];
							so[icol*this->nxyz + ixyz] = dbuffer[icol*nrow_x + irow];
						}
					}
				}
			}
			local_start_cell += nrow_x;
		}
#endif
	}
	catch (...)
	{
		return_value = IRM_FAIL;
	}
	return this->ReturnHandler(return_value, "PhreeqcRM::GetSelectedOutput");
}
/* ---------------------------------------------------------------------- */
int
PhreeqcRM::GetSelectedOutputColumnCount()
/* ---------------------------------------------------------------------- */
{
	this->phreeqcrm_error_string.clear();
	try
	{
		if (this->workers[0]->CurrentSelectedOutputUserNumber >= 0)
		{
			std::map< int, CSelectedOutput >::iterator it = this->workers[0]->CSelectedOutputMap.find(
				this->workers[0]->CurrentSelectedOutputUserNumber);
			if (it != this->workers[0]->CSelectedOutputMap.end())
			{
				return (int) it->second.GetColCount();
			}
		}
		this->ErrorHandler(IRM_INVALIDARG, "Selected output not found.");
	}
	catch (...)
	{
	}
	return this->ReturnHandler(IRM_INVALIDARG, "PhreeqcRM::GetSelectedOutputColumnCount");
}

/* ---------------------------------------------------------------------- */
int
PhreeqcRM::GetSelectedOutputCount(void)
/* ---------------------------------------------------------------------- */
{
	this->phreeqcrm_error_string.clear();
	return (int) this->workers[0]->CSelectedOutputMap.size();
}
/* ---------------------------------------------------------------------- */
IRM_RESULT
PhreeqcRM::GetSelectedOutputHeading(int icol, std::string &heading)
/* ---------------------------------------------------------------------- */
{
	this->phreeqcrm_error_string.clear();
	try
	{
		if (this->workers[0]->CurrentSelectedOutputUserNumber >= 0)
		{
			std::map< int, CSelectedOutput >::iterator it = this->workers[0]->CSelectedOutputMap.find(
				this->workers[0]->CurrentSelectedOutputUserNumber);
			if (it != this->workers[0]->CSelectedOutputMap.end())
			{
				VAR pVar;
				VarInit(&pVar);
				if (it->second.Get(0, icol, &pVar) == VR_OK)
				{
					if (pVar.type == TT_STRING)
					{
						heading = pVar.sVal;
						VarClear(&pVar);
						return IRM_OK;
					}
				}
				VarClear(&pVar);
			}
		}
		else
		{
			this->ErrorHandler(IRM_INVALIDARG, "Selected output not found.");
		}
	}
	catch (...)
	{
	}
	return this->ReturnHandler(IRM_INVALIDARG, "PhreeqcRM::GetSelectedOutputHeading");
}
/* ---------------------------------------------------------------------- */
IRM_RESULT
PhreeqcRM::GetSelectedOutputHeadings(std::vector<std::string>& headings)
/* ---------------------------------------------------------------------- */
{
	this->phreeqcrm_error_string.clear();
	headings.clear();
	try
	{
		int ncol = this->GetSelectedOutputColumnCount();
		if (ncol >= 0) 
		{
			for (size_t i = 0; i < ncol; i++)
			{
				std::string heading;
				this->GetSelectedOutputHeading(i, heading);
				headings.push_back(heading);
			}
			return IRM_OK;
		}
		else
		{
			this->ErrorHandler(IRM_INVALIDARG, "Selected output not found.");
		}
	}
	catch (...)
	{
	}
	return this->ReturnHandler(IRM_INVALIDARG, "PhreeqcRM::GetSelectedOutputHeadings");
}
/* ---------------------------------------------------------------------- */
int
PhreeqcRM::GetSelectedOutputRowCount()
/* ---------------------------------------------------------------------- */
{
	this->phreeqcrm_error_string.clear();
	return this->nxyz;
}
/* ---------------------------------------------------------------------- */
const std::vector<double> &
PhreeqcRM::GetSolutionVolume(void)
/* ---------------------------------------------------------------------- */
{
	this->phreeqcrm_error_string.clear();
	try
	{
#ifdef USE_MPI
		if (this->mpi_myself == 0)
		{
			int method = METHOD_GETSOLUTIONVOLUME;
			MPI_Bcast(&method, 1, MPI_INT, 0, phreeqcrm_comm);
		}
		int size = this->end_cell[this->mpi_myself] - this->start_cell[this->mpi_myself] + 1;
		this->solution_volume_worker.resize(size, INACTIVE_CELL_VALUE);
		
		// fill solution_volume
		int n = this->mpi_myself;
		for (int i = this->start_cell[n]; i <= this->end_cell[n]; i++)
		{
			this->solution_volume_worker[i - this->start_cell[n]] = this->workers[0]->Get_solution(i)->Get_soln_vol();;
		}
		// Gather to root
		GatherNchem(this->solution_volume_worker, this->solution_volume_root);
#else
		this->solution_volume_root.resize(this->nxyz, INACTIVE_CELL_VALUE);
		std::vector<double> dbuffer;
		for (int n = 0; n < this->nthreads; n++)
		{
			for (int i = start_cell[n]; i <= this->end_cell[n]; i++)
			{
				double d = this->workers[n]->Get_solution(i)->Get_soln_vol();
				for(size_t j = 0; j < backward_mapping[i].size(); j++)
				{
					int n = backward_mapping[i][j];
					this->solution_volume_root[n] = d;
				}
			}
		}
#endif
	}
	catch (...)
	{
		this->ReturnHandler(IRM_FAIL, "PhreeqcRM::GetSolutionVolume");
		this->solution_volume_root.clear();
		this->solution_volume_worker.clear();
	}
	return this->solution_volume_root;
}
#ifdef USE_MPI
/* ---------------------------------------------------------------------- */
IRM_RESULT
PhreeqcRM::GetSpeciesConcentrations(std::vector<double> & species_conc)
/* ---------------------------------------------------------------------- */
{
	this->phreeqcrm_error_string.clear();
	if (this->mpi_myself == 0)
	{
		int method = METHOD_GETSPECIESCONCENTRATIONS;
		MPI_Bcast(&method, 1, MPI_INT, 0, phreeqcrm_comm);
	}

	if (this->species_save_on)
	{
		size_t nspecies = this->species_names.size();
		// Fill in root concentrations
		if (this->mpi_myself == 0)
		{
			species_conc.resize(nspecies * this->nxyz, 0);
			for (int j = this->start_cell[0]; j <= this->end_cell[0]; j++)
			{
				std::vector<double> d;
				d.resize(this->species_names.size(), 0);
				{
					std::map<int,double>::iterator it = this->workers[0]->Get_solution(j)->Get_species_map().begin();
					for ( ; it != this->workers[0]->Get_solution(j)->Get_species_map().end(); it++)
					{
						// it is pointing to a species number, concentration
						int rm_species_num = this->s_num2rm_species_num[it->first];
						d[rm_species_num] = it->second;
					}
				}
				{
					std::vector<int>::iterator it;
					for (it = this->backward_mapping[j].begin(); it != this->backward_mapping[j].end(); it++)
					{
						double *d_ptr = &species_conc[*it];
						for (size_t i = 0; i < d.size(); i++)
						{
							d_ptr[this->nxyz * i] = d[i];
						}
					}
				}
			}
		}
		// Fill in worker concentrations
		for (int n = 1; n < this->mpi_tasks; n++)
		{
			int ncells = this->end_cell[n] - start_cell[n] + 1;
			if (this->mpi_myself == n)
			{
				species_conc.resize(nspecies * ncells, 0);
				for (int j = this->start_cell[n]; j <= this->end_cell[n]; j++)
				{
					int j0 = j - this->start_cell[n];
					{
						std::map<int,double>::iterator it = this->workers[0]->Get_solution(j)->Get_species_map().begin();
						for ( ; it != this->workers[0]->Get_solution(j)->Get_species_map().end(); it++)
						{
							// it is pointing to a species number, concentration
							int rm_species_num = this->s_num2rm_species_num[it->first];
							species_conc[rm_species_num * ncells + j0] = it->second;
						}
					}
				}
				MPI_Send((void *) &species_conc.front(), (int) nspecies * ncells, MPI_DOUBLE, 0, 0, phreeqcrm_comm);
			}
			else if (this->mpi_myself == 0)
			{
				MPI_Status mpi_status;
				double * recv_species = new double[(size_t)  nspecies * ncells];
				MPI_Recv(recv_species, (int) nspecies * ncells, MPI_DOUBLE, n, 0, phreeqcrm_comm, &mpi_status);
				for (int j = this->start_cell[n]; j <= this->end_cell[n]; j++)
				{
					int j0 = j - this->start_cell[n];
					std::vector<int>::iterator it;
					for (it = this->backward_mapping[j].begin(); it != this->backward_mapping[j].end(); it++)
					{
						double *d_ptr = &species_conc[*it];
						for (size_t i = 0; i < nspecies; i++)
						{
							d_ptr[this->nxyz * i] = recv_species[i * ncells + j0];
						}
					}
				}
				delete[] recv_species;
			}
		}
	}
	else
	{
		species_conc.clear();
	}
	return IRM_OK;
}
#else
/* ---------------------------------------------------------------------- */
IRM_RESULT
PhreeqcRM::GetSpeciesConcentrations(std::vector<double> & species_conc)
/* ---------------------------------------------------------------------- */
{
	this->phreeqcrm_error_string.clear();
	if (this->species_save_on)
	{
		size_t nspecies = this->species_names.size();
		species_conc.resize(nspecies * this->nxyz, 0);
		for (int i = 0; i < this->nthreads; i++)
		{
			for (int j = this->start_cell[i]; j <= this->end_cell[i]; j++)
			{
				std::vector<double> d;
				d.resize(this->species_names.size(), 0);
				{
					std::map<int,double>::iterator it = this->workers[i]->Get_solution(j)->Get_species_map().begin();
					for ( ; it != this->workers[i]->Get_solution(j)->Get_species_map().end(); it++)
					{
						// it is pointing to a species number, concentration
						int rm_species_num = this->s_num2rm_species_num[it->first];
						d[rm_species_num] = it->second;
					}
				}
				std::vector<int>::iterator it;
				for (it = this->backward_mapping[j].begin(); it != this->backward_mapping[j].end(); it++)
				{
					double *d_ptr = &species_conc[*it];
					for (size_t i = 0; i < d.size(); i++)
					{
						d_ptr[this->nxyz * i] = d[i];
					}
				}
			}
		}
	}
	else
	{
		species_conc.clear();
	}
	return IRM_OK;
}
#endif
#ifdef USE_MPI
/* ---------------------------------------------------------------------- */
IRM_RESULT
PhreeqcRM::GetSpeciesLog10Gammas(std::vector<double> & species_log10gammas)
/* ---------------------------------------------------------------------- */
{
	this->phreeqcrm_error_string.clear();
	if (this->mpi_myself == 0)
	{
		int method = METHOD_GETSPECIESLOG10GAMMAS;
		MPI_Bcast(&method, 1, MPI_INT, 0, phreeqcrm_comm);
	}

	if (this->species_save_on)
	{
		size_t nspecies = this->species_names.size();
		// Fill in root concentrations
		if (this->mpi_myself == 0)
		{
			species_log10gammas.resize(nspecies * this->nxyz, 0);
			for (int j = this->start_cell[0]; j <= this->end_cell[0]; j++)
			{
				std::vector<double> d;
				d.resize(this->species_names.size(), 0);
				{
					std::map<int, double>::iterator it = this->workers[0]->Get_solution(j)->Get_log_gamma_map().begin();
					for (; it != this->workers[0]->Get_solution(j)->Get_log_gamma_map().end(); it++)
					{
						// it is pointing to a species number, concentration
						int rm_species_num = this->s_num2rm_species_num[it->first];
						d[rm_species_num] = it->second;
					}
				}
				{
					std::vector<int>::iterator it;
					for (it = this->backward_mapping[j].begin(); it != this->backward_mapping[j].end(); it++)
					{
						double *d_ptr = &species_log10gammas[*it];
						for (size_t i = 0; i < d.size(); i++)
						{
							d_ptr[this->nxyz * i] = d[i];
						}
					}
				}
			}
		}
		// Fill in worker concentrations
		for (int n = 1; n < this->mpi_tasks; n++)
		{
			int ncells = this->end_cell[n] - start_cell[n] + 1;
			if (this->mpi_myself == n)
			{
				species_log10gammas.resize(nspecies * ncells, 0);
				for (int j = this->start_cell[n]; j <= this->end_cell[n]; j++)
				{
					int j0 = j - this->start_cell[n];
					{
						std::map<int, double>::iterator it = this->workers[0]->Get_solution(j)->Get_log_gamma_map().begin();
						for (; it != this->workers[0]->Get_solution(j)->Get_log_gamma_map().end(); it++)
						{
							// it is pointing to a species number, concentration
							int rm_species_num = this->s_num2rm_species_num[it->first];
							species_log10gammas[rm_species_num * ncells + j0] = it->second;
						}
					}
				}
				MPI_Send((void *)&species_log10gammas.front(), (int)nspecies * ncells, MPI_DOUBLE, 0, 0, phreeqcrm_comm);
			}
			else if (this->mpi_myself == 0)
			{
				MPI_Status mpi_status;
				double * recv_species = new double[(size_t)nspecies * ncells];
				MPI_Recv(recv_species, (int)nspecies * ncells, MPI_DOUBLE, n, 0, phreeqcrm_comm, &mpi_status);
				for (int j = this->start_cell[n]; j <= this->end_cell[n]; j++)
				{
					int j0 = j - this->start_cell[n];
					std::vector<int>::iterator it;
					for (it = this->backward_mapping[j].begin(); it != this->backward_mapping[j].end(); it++)
					{
						double *d_ptr = &species_log10gammas[*it];
						for (size_t i = 0; i < nspecies; i++)
						{
							d_ptr[this->nxyz * i] = recv_species[i * ncells + j0];
						}
					}
				}
				delete[] recv_species;
			}
		}
	}
	else
	{
		species_log10gammas.clear();
	}
	return IRM_OK;
}
#else
/* ---------------------------------------------------------------------- */
IRM_RESULT
PhreeqcRM::GetSpeciesLog10Gammas(std::vector<double> & species_log10gammas)
/* ---------------------------------------------------------------------- */
{
	this->phreeqcrm_error_string.clear();
	if (this->species_save_on)
	{
		size_t nspecies = this->species_names.size();
		species_log10gammas.resize(nspecies * this->nxyz, 0);
		for (int i = 0; i < this->nthreads; i++)
		{
			for (int j = this->start_cell[i]; j <= this->end_cell[i]; j++)
			{
				std::vector<double> d;
				d.resize(this->species_names.size(), 0);
				{
					std::map<int, double>::iterator it = this->workers[i]->Get_solution(j)->Get_log_gamma_map().begin();
					for (; it != this->workers[i]->Get_solution(j)->Get_log_gamma_map().end(); it++)
					{
						// it is pointing to a species number, concentration
						int rm_species_num = this->s_num2rm_species_num[it->first];
						d[rm_species_num] = it->second;
					}
				}
				std::vector<int>::iterator it;
				for (it = this->backward_mapping[j].begin(); it != this->backward_mapping[j].end(); it++)
				{
					double *d_ptr = &species_log10gammas[*it];
					for (size_t i = 0; i < d.size(); i++)
					{
						d_ptr[this->nxyz * i] = d[i];
					}
				}
			}
		}
	}
	else
	{
		species_log10gammas.clear();
	}
	return IRM_OK;
}
#endif

#ifdef USE_MPI
/* ---------------------------------------------------------------------- */
IRM_RESULT
PhreeqcRM::GetSpeciesLog10Molalities(std::vector<double>& species_log10molalities)
/* ---------------------------------------------------------------------- */
{
	this->phreeqcrm_error_string.clear();
	if (this->mpi_myself == 0)
	{
		int method = METHOD_GETSPECIESLOG10MOLALITIES;
		MPI_Bcast(&method, 1, MPI_INT, 0, phreeqcrm_comm);
	}

	if (this->species_save_on)
	{
		size_t nspecies = this->species_names.size();
		// Fill in root concentrations
		if (this->mpi_myself == 0)
		{
			species_log10molalities.resize(nspecies * this->nxyz, 0);
			for (int j = this->start_cell[0]; j <= this->end_cell[0]; j++)
			{
				std::vector<double> d;
				d.resize(this->species_names.size(), 0);
				{
					std::map<int, double>::iterator it = this->workers[0]->Get_solution(j)->Get_log_molalities_map().begin();
					for (; it != this->workers[0]->Get_solution(j)->Get_log_molalities_map().end(); it++)
					{
						// it is pointing to a species number, concentration
						int rm_species_num = this->s_num2rm_species_num[it->first];
						d[rm_species_num] = it->second;
					}
				}
				{
					std::vector<int>::iterator it;
					for (it = this->backward_mapping[j].begin(); it != this->backward_mapping[j].end(); it++)
					{
						double* d_ptr = &species_log10molalities[*it];
						for (size_t i = 0; i < d.size(); i++)
						{
							d_ptr[this->nxyz * i] = d[i];
						}
					}
				}
			}
		}
		// Fill in worker concentrations
		for (int n = 1; n < this->mpi_tasks; n++)
		{
			int ncells = this->end_cell[n] - start_cell[n] + 1;
			if (this->mpi_myself == n)
			{
				species_log10molalities.resize(nspecies * ncells, 0);
				for (int j = this->start_cell[n]; j <= this->end_cell[n]; j++)
				{
					int j0 = j - this->start_cell[n];
					{
						std::map<int, double>::iterator it = this->workers[0]->Get_solution(j)->Get_log_molalities_map().begin();
						for (; it != this->workers[0]->Get_solution(j)->Get_log_molalities_map().end(); it++)
						{
							// it is pointing to a species number, concentration
							int rm_species_num = this->s_num2rm_species_num[it->first];
							species_log10molalities[rm_species_num * ncells + j0] = it->second;
						}
					}
				}
				MPI_Send((void*)&species_log10molalities.front(), (int)nspecies * ncells, MPI_DOUBLE, 0, 0, phreeqcrm_comm);
			}
			else if (this->mpi_myself == 0)
			{
				MPI_Status mpi_status;
				double* recv_species = new double[(size_t)nspecies * ncells];
				MPI_Recv(recv_species, (int)nspecies * ncells, MPI_DOUBLE, n, 0, phreeqcrm_comm, &mpi_status);
				for (int j = this->start_cell[n]; j <= this->end_cell[n]; j++)
				{
					int j0 = j - this->start_cell[n];
					std::vector<int>::iterator it;
					for (it = this->backward_mapping[j].begin(); it != this->backward_mapping[j].end(); it++)
					{
						double* d_ptr = &species_log10molalities[*it];
						for (size_t i = 0; i < nspecies; i++)
						{
							d_ptr[this->nxyz * i] = recv_species[i * ncells + j0];
						}
					}
				}
				delete recv_species;
			}
		}
	}
	else
	{
		species_log10molalities.clear();
	}
	return IRM_OK;
}
#else
/* ---------------------------------------------------------------------- */
IRM_RESULT
PhreeqcRM::GetSpeciesLog10Molalities(std::vector<double>& species_log10molalities)
/* ---------------------------------------------------------------------- */
{
	this->phreeqcrm_error_string.clear();
	if (this->species_save_on)
	{
		size_t nspecies = this->species_names.size();
		species_log10molalities.resize(nspecies * this->nxyz, 0);
		for (int i = 0; i < this->nthreads; i++)
		{
			for (int j = this->start_cell[i]; j <= this->end_cell[i]; j++)
			{
				std::vector<double> d;
				d.resize(this->species_names.size(), 0);
				{
					std::map<int, double>::iterator it = this->workers[i]->Get_solution(j)->Get_log_molalities_map().begin();
					for (; it != this->workers[i]->Get_solution(j)->Get_log_molalities_map().end(); it++)
					{
						// it is pointing to a species number, concentration
						int rm_species_num = this->s_num2rm_species_num[it->first];
						d[rm_species_num] = it->second;
					}
				}
				std::vector<int>::iterator it;
				for (it = this->backward_mapping[j].begin(); it != this->backward_mapping[j].end(); it++)
				{
					double* d_ptr = &species_log10molalities[*it];
					for (size_t i = 0; i < d.size(); i++)
					{
						d_ptr[this->nxyz * i] = d[i];
					}
				}
			}
		}
	}
	else
	{
		species_log10molalities.clear();
	}
	return IRM_OK;
}
void 
PhreeqcRM::GetSpeciesStoichiometrySWIG(std::vector<std::string>& species, std::vector<int>& nelt_in_species, \
	std::vector<std::string>& elts, std::vector<double>& coef)
{
	std::vector<class cxxNameDouble> stoichiometry = this->GetSpeciesStoichiometry();
	species = this->GetSpeciesNames();
	for (size_t i = 0; i < species.size(); i++)
	{
		cxxNameDouble s_stoich = stoichiometry[i];
		nelt_in_species.push_back((int)s_stoich.size());
		cxxNameDouble::iterator it = s_stoich.begin();
		for (; it != s_stoich.end(); it++)
		{
			elts.push_back(it->first);
			coef.push_back(it->second);
		}
	}
}
#endif

#ifdef USE_MPI
/* ---------------------------------------------------------------------- */
const std::vector<double> &
PhreeqcRM::GetTemperature(void)
/* ---------------------------------------------------------------------- */
{
	this->phreeqcrm_error_string.clear();
	try
	{
		if (this->mpi_myself == 0)
		{
			int method = METHOD_GETTEMPERATURE;
			MPI_Bcast(&method, 1, MPI_INT, 0, phreeqcrm_comm);
		}
		int size = this->end_cell[this->mpi_myself] - this->start_cell[this->mpi_myself] + 1;
		this->tempc_worker.resize(size, INACTIVE_CELL_VALUE);
		
		// fill tempc
		int n = this->mpi_myself;
		for (int i = this->start_cell[n]; i <= this->end_cell[n]; i++)
		{
			this->tempc_worker[i - this->start_cell[n]] = this->workers[0]->Get_solution(i)->Get_tc();
		}
		
		// Gather to root
		GatherNchem(this->tempc_worker, this->tempc_root);

	}
	catch (...)
	{
		this->ReturnHandler(IRM_FAIL, "PhreeqcRM::GetTemperature");
		this->tempc_worker.clear();
		this->tempc_root.clear();
	}
	return this->tempc_root;
}
#else
/* ---------------------------------------------------------------------- */
const std::vector<double> &
PhreeqcRM::GetTemperature(void)
/* ---------------------------------------------------------------------- */
{
	this->phreeqcrm_error_string.clear();
	try
	{

		this->tempc_root.resize(this->nxyz, INACTIVE_CELL_VALUE);
		std::vector<double> dbuffer;

		for (int n = 0; n < this->nthreads; n++)
		{
			for (int i = start_cell[n]; i <= this->end_cell[n]; i++)
			{
				if (this->workers[n]->Get_solution(i))
				{
					double d = this->workers[n]->Get_solution(i)->Get_tc();
					for(size_t j = 0; j < backward_mapping[i].size(); j++)
					{
						int n = backward_mapping[i][j];
						this->tempc_root[n] = d;
					}
				}
				else
				{
					std::ostringstream e_stream;
				    e_stream << "Solution not found in GetTemperatures " << i << std::endl;
					this->ErrorMessage(e_stream.str());
				}
			}
		}
	}
	catch (...)
	{
		this->ReturnHandler(IRM_FAIL, "PhreeqcRM::GetTemperature");
		this->tempc_root.clear();
		this->tempc_worker.clear();
	}
	return this->tempc_root;
}
#endif
//int PhreeqcRM::GetVarItemsize(const std::string name)
//{
//	RMVARS v_enum = this->phreeqcrm_var_man->GetEnum(name);
//	if (v_enum != RMVARS::NotFound)
//	{
//		BMIVariant& bv = this->phreeqcrm_var_man->VariantMap[v_enum];
//		if (!bv.GetInitialized())
//		{
//			this->phreeqcrm_var_man->task = VarManager::VAR_TASKS::Info;
//			((*this->phreeqcrm_var_man).*bv.GetFn())();
//		}
//		return bv.GetItemsize();
//	}
//	assert(false);
//	return 0;
//}
//int PhreeqcRM::GetVarNbytes(const std::string name)
//{
//	RMVARS v_enum = this->phreeqcrm_var_man->GetEnum(name);
//	if (v_enum != RMVARS::NotFound)
//	{
//		BMIVariant& bv = this->phreeqcrm_var_man->VariantMap[v_enum];
//		if (!bv.GetInitialized())
//		{
//			this->phreeqcrm_var_man->task = VarManager::VAR_TASKS::Info;
//			((*this->phreeqcrm_var_man).*bv.GetFn())();
//		}
//		return bv.GetNbytes();
//	}
//	assert(false);
//	return 0;
//}
/* ---------------------------------------------------------------------- */
const std::vector<double>&
PhreeqcRM::GetViscosity(void)
/* ---------------------------------------------------------------------- */
{
	this->phreeqcrm_error_string.clear();
	try
	{
#ifdef USE_MPI
		if (this->mpi_myself == 0)
		{
			int method = METHOD_GETSOLUTIONVOLUME;
			MPI_Bcast(&method, 1, MPI_INT, 0, phreeqcrm_comm);
		}
		int size = this->end_cell[this->mpi_myself] - this->start_cell[this->mpi_myself] + 1;
		this->viscosity_worker.resize(size, INACTIVE_CELL_VALUE);

		// fill viscosity
		int n = this->mpi_myself;
		for (int i = this->start_cell[n]; i <= this->end_cell[n]; i++)
		{
			this->viscosity_worker[i - this->start_cell[n]] = this->workers[0]->Get_solution(i)->Get_viscosity();;
		}
		// Gather to root
		GatherNchem(this->viscosity_worker, this->viscosity_root);
#else
		this->viscosity_root.resize(this->nxyz, INACTIVE_CELL_VALUE);
		std::vector<double> dbuffer;
		for (int n = 0; n < this->nthreads; n++)
		{
			for (int i = start_cell[n]; i <= this->end_cell[n]; i++)
			{
				double d = this->workers[n]->Get_solution(i)->Get_viscosity();
				for (size_t j = 0; j < backward_mapping[i].size(); j++)
				{
					int n = backward_mapping[i][j];
					this->viscosity_root[n] = d;
				}
			}
		}
#endif
	}
	catch (...)
	{
		this->ReturnHandler(IRM_FAIL, "PhreeqcRM::GetViscosity");
		this->viscosity_root.clear();
		this->viscosity_worker.clear();
	}
	return this->viscosity_root;
}

#ifdef USE_MPI
/* ---------------------------------------------------------------------- */
IRM_RESULT
PhreeqcRM::HandleErrorsInternal(std::vector <int> & r_vector)
/* ---------------------------------------------------------------------- */
{
	// Check for errors
	std::vector<int> recv_buffer;
	recv_buffer.resize(this->mpi_tasks);
	MPI_Gather(&r_vector[0], 1, MPI_INT, &recv_buffer.front(), 1, MPI_INT, 0, phreeqcrm_comm);

	// Determine whether there were errors
	this->error_count = 0;
	if (mpi_myself == 0)
	{
		for (int n = 0; n < this->mpi_tasks; n++)
		{
			if (recv_buffer[n] != 0)
				this->error_count++;
		}
	}
	MPI_Bcast(&this->error_count, 1, MPI_INT, 0, phreeqcrm_comm);
	// return if no errors
	if (error_count == 0) return IRM_OK;
	// Root write any error messages
	for (int n = 0; n < this->mpi_tasks; n++)
	{
		if (mpi_myself == n)
		{
			if (mpi_myself == 0)
			{
				if (recv_buffer[n] != 0)
				{
					// print error
					std::ostringstream e_stream;
				    e_stream << "Process " << n << std::endl;
					this->ErrorMessage(e_stream.str());
					this->ErrorMessage(this->workers[0]->GetErrorString(), false);
				}
			}
			else
			{
				if (r_vector[0] != 0)
				{
					// send error
					int size = (int) strlen(this->workers[0]->GetErrorString());
					MPI_Send(&size, 1, MPI_INT, 0, 0, phreeqcrm_comm);
					MPI_Send((void *) this->workers[0]->GetErrorString(), size, MPI_CHAR, 0, 0, phreeqcrm_comm);
				}
			}
		}
		else if (mpi_myself == 0)
		{
			if (recv_buffer[n] != 0)
			{
				std::ostringstream e_stream;
				e_stream << "Process " << n << std::endl;
				this->ErrorMessage(e_stream.str());
				MPI_Status mpi_status;
				// receive and print error
				int size;
				MPI_Recv(&size, 1, MPI_INT, n, 0, phreeqcrm_comm, &mpi_status);
				std::string char_buffer;
				char_buffer.resize(size + 1);
				MPI_Recv((void *) &char_buffer[0], size, MPI_CHAR, n, 0, phreeqcrm_comm, &mpi_status);
				char_buffer[size] = '\0';
				this->ErrorMessage(char_buffer, false);
			}
		}
	}
	MPI_Barrier(phreeqcrm_comm);
	throw PhreeqcRMStop();
}
#else
/* ---------------------------------------------------------------------- */
IRM_RESULT
PhreeqcRM::HandleErrorsInternal(std::vector< int > &rtn)
/* ---------------------------------------------------------------------- */
{
	// Check for errors
	this->error_count = 0;

	// Write error messages
	for (size_t n = 0; n < rtn.size(); n++)
	{
		if (rtn[n] != 0)
		{
			this->ErrorMessage(this->workers[n]->GetErrorString(), false);
			this->error_count++;
		}
	}
	if (error_count > 0)
		throw PhreeqcRMStop();
	return IRM_OK;
}
#endif
#ifdef USE_YAML
IRM_RESULT		PhreeqcRM::InitializeYAML(std::string config)
{
	if (config.size() > 0)
	{
		YAML::Node yaml = YAML::LoadFile(config);
		//std::string keyword;
		//YAML::Node node;
		for (YAML::Node::const_iterator it = yaml.begin();it != yaml.end();++it)
		{
			YAML::Node node = *it;
			auto it1 = node.begin();
			assert(node.IsMap());
			std::string keyword = it1++->second.as<std::string>();
			if (keyword == "AddOutputVars")
			{
				assert(node.size() == 3);
				std::string option = it1++->second.as<std::string>();
				std::string def = it1++->second.as<std::string>();
				this->AddOutputVars(option, def);
				continue;
			}
			if (keyword == "CloseFiles")
			{
				this->CloseFiles();
				continue;
			}
			if (keyword == "CreateMapping")
			{
				std::vector<int> grid2chem = it1++->second.as<std::vector<int>>();
				this->CreateMapping(grid2chem);
				continue;
			}
			if (keyword == "DumpModule")
			{
				assert(node.size() == 3);
				bool dump_on = it1++->second.as<bool>();
				bool append = it1++->second.as<bool>();
				this->DumpModule(dump_on, append);
				continue;
			}
			if (keyword == "FindComponents")
			{
				this->FindComponents();
				continue;
			}

			if (keyword == "InitialSolutions2Module")
			{
				std::vector< int > ic = it1++->second.as< std::vector< int > >();
				this->InitialSolutions2Module(ic);
				continue;
			}
			if (keyword == "InitialEquilibriumPhases2Module")
			{
				std::vector< int > ic = it1++->second.as< std::vector< int > >();
				this->InitialEquilibriumPhases2Module(ic);
				continue;
			}
			if (keyword == "InitialExchanges2Module")
			{
				std::vector< int > ic = it1++->second.as< std::vector< int > >();
				this->InitialExchanges2Module(ic);
				continue;
			}
			if (keyword == "InitialSurfaces2Module")
			{
				std::vector< int > ic = it1++->second.as< std::vector< int > >();
				this->InitialSurfaces2Module(ic);
				continue;
			}
			if (keyword == "InitialGasPhases2Module")
			{
				std::vector< int > ic = it1++->second.as< std::vector< int > >();
				this->InitialGasPhases2Module(ic);
				continue;
			}
			if (keyword == "InitialSolidSolutions2Module")
			{
				std::vector< int > ic = it1++->second.as< std::vector< int > >();
				this->InitialSolidSolutions2Module(ic);
				continue;
			}
			if (keyword == "InitialKinetics2Module")
			{
				std::vector< int > ic = it1++->second.as< std::vector< int > >();
				this->InitialKinetics2Module(ic);
				continue;
			}

			if (keyword == "InitialPhreeqc2Module")
			{
				if (node.size() == 2)
				{
					std::vector< int > ic = it1++->second.as< std::vector< int > >();
					this->InitialPhreeqc2Module(ic);
					continue;
				}
				else if (node.size() == 4)
				{
					std::vector < int > ic1 = it1++->second.as< std::vector < int > >();
					std::vector < int > ic2 = it1++->second.as< std::vector < int > >();
					std::vector < double > f1 = it1->second.as< std::vector < double > >();
					this->InitialPhreeqc2Module(ic1, ic2, f1);
					continue;
				}
				//throw LetItThrow("YAML argument mismatch InitialPhreeqc2Module");
				ErrorMessage("YAML argument mismatch InitialPhreeqc2Module");
				throw PhreeqcRMStop();
			}
			if (keyword == "InitialPhreeqc2Module_mix")
			{
				if (node.size() == 4)
				{
					std::vector < int > ic1 = it1++->second.as< std::vector < int > >();
					std::vector < int > ic2 = it1++->second.as< std::vector < int > >();
					std::vector < double > f1 = it1->second.as< std::vector < double > >();
					this->InitialPhreeqc2Module(ic1, ic2, f1);
					continue;
				}
				//throw LetItThrow("YAML argument mismatch InitialPhreeqc2Module");
				ErrorMessage("YAML argument mismatch InitialPhreeqc2Module_mix");
				throw PhreeqcRMStop();
			}
			if (keyword == "InitialPhreeqcCell2Module")
			{
				assert(node.size() == 3);
				int n = it1++->second.as<int>();
				std::vector< int > cell_numbers = it1++->second.as< std::vector<int > >();
				this->InitialPhreeqcCell2Module(n, cell_numbers);
				continue;
			}
			if (keyword == "LoadDatabase")
			{
				std::string file = it1++->second.as< std::string >();
				// no need to check for initialization just call base class
				this->PhreeqcRM::LoadDatabase(file);
				continue;
			}
			if (keyword == "OpenFiles")
			{
				this->OpenFiles();
				continue;
			}
			if (keyword == "OutputMessage")
			{
				std::string str = it1++->second.as< std::string >();
				this->OutputMessage(str);
				continue;
			}
			if (keyword == "RunCells")
			{
				this->RunCells();
				continue;
			}
			if (keyword == "RunFile")
			{
				assert(node.size() == 5);
				bool workers = it1++->second.as<bool>();
				bool initial = it1++->second.as<bool>();
				bool utility = it1++->second.as<bool>();
				std::string  file = it1++->second.as< std::string >();
				this->RunFile(workers, initial, utility, file);
				continue;
			}
			if (keyword == "RunString")
			{
				assert(node.size() == 5);
				bool workers = it1++->second.as<bool>();
				bool initial = it1++->second.as<bool>();
				bool utility = it1++->second.as<bool>();
				std::string  string = it1++->second.as<std::string>();
				this->RunString(workers, initial, utility, string);
				continue;
			}
			if (keyword == "ScreenMessage")
			{
				std::string str = it1++->second.as< std::string >();
				this->ScreenMessage(str);
				continue;
			}
			if (keyword == "SetComponentH2O")
			{
				bool tf = it1++->second.as< bool >();
				this->SetComponentH2O(tf);
				continue;
			}
			if (keyword == "SetConcentrations")
			{
				std::vector< double > c = it1++->second.as< std::vector< double > >();
				this->SetConcentrations(c);
				continue;
			}
			if (keyword == "SetCurrentSelectedOutputUserNumber")
			{
				int n = it1++->second.as< int >();
				this->SetCurrentSelectedOutputUserNumber(n);
				continue;
			}
			if (keyword == "SetDensityUser")
			{
				std::vector< double > den = it1++->second.as< std::vector< double > >();
				this->SetDensityUser(den);
				continue;
			}
			if (keyword == "SetDumpFileName")
			{
				std::string str = it1++->second.as< std::string >();
				this->SetDumpFileName(str);
				continue;
			}
			if (keyword == "SetErrorHandlerMode")
			{
				int mode = it1++->second.as< int >();
				this->SetErrorHandlerMode(mode);
				continue;
			}
			if (keyword == "SetErrorOn")
			{
				bool tf = it1++->second.as< bool >();
				this->SetErrorOn(tf);
				continue;
			}
			if (keyword == "SetFilePrefix")
			{
				std::string prefix = it1++->second.as< std::string >();
				this->SetFilePrefix(prefix);
				continue;
			}
			if (keyword == "SetGasCompMoles")
			{
				std::vector<double> mol = it1++->second.as< std::vector < double > >();
				this->SetGasCompMoles(mol);
				continue;
			}
			if (keyword == "SetGasPhaseVolume")
			{
				std::vector<double> vol = it1++->second.as< std::vector < double > >();
				this->SetGasPhaseVolume(vol);
				continue;
			}
			if (keyword == "SetGridCellCount")
			{
				//this->WarningMessage("SetGridCellCount has no effect after the PhreeqcRM instance is created.");
				continue;
			}
			if (keyword == "SetPartitionUZSolids")
			{
				bool tf = it1++->second.as< bool >();
				this->SetPartitionUZSolids(tf);
				continue;
			}
			if (keyword == "SetPorosity")
			{
				std::vector<double> por = it1++->second.as< std::vector< double > >();
				this->SetPorosity(por);
				continue;
			}
			if (keyword == "SetPressure")
			{
				std::vector<double> pressure = it1++->second.as< std::vector< double > >();
				this->SetPressure(pressure);
				continue;
			}
			if (keyword == "SetPrintChemistryMask")
			{
				std::vector<int> mask = it1++->second.as< std::vector< int > >();
				this->SetPrintChemistryMask(mask);
				continue;
			}
			if (keyword == "SetPrintChemistryOn")
			{
				assert(node.size() == 4);
				bool workers = it1++->second.as< bool >();
				bool initial = it1++->second.as< bool >();
				bool utility = it1++->second.as< bool >();
				this->SetPrintChemistryOn(workers, initial, utility);
				continue;
			}
			if (keyword == "SetRebalanceByCell")
			{
				bool tf = it1++->second.as< bool >();
				this->SetRebalanceByCell(tf);
				continue;
			}
			if (keyword == "SetRebalanceFraction")
			{
				double f = it1++->second.as<double>();
				this->SetRebalanceFraction(f);
				continue;
			}
			if (keyword == "SetRepresentativeVolume")
			{
				std::vector<double> rv = it1++->second.as< std::vector<double > >();
				this->SetRepresentativeVolume(rv);
				continue;
			}
			if (keyword == "SetSaturationUser")
			{
				std::vector< double > sat = it1++->second.as< std::vector< double> >();
				this->SetSaturationUser(sat);
				continue;
			}
			if (keyword == "SetScreenOn")
			{
				bool tf = it1++->second.as< bool >();
				this->SetScreenOn(tf);
				continue;
			}
			if (keyword == "SetSelectedOutputOn")
			{
				bool tf = it1++->second.as< bool >();
				this->SetSelectedOutputOn(tf);
				continue;
			}
			if (keyword == "SetSpeciesSaveOn")
			{
				bool tf = it1++->second.as< bool >();
				this->SetSpeciesSaveOn(tf);
				continue;
			}
			if (keyword == "SetTemperature")
			{
				std::vector<double> temp = it1++->second.as<std::vector<double>>();
				this->SetTemperature(temp);
				continue;
			}
			if (keyword == "SetTime")
			{
				double time = it1++->second.as<double>();
				this->SetTime(time);
				continue;
			}
			if (keyword == "SetTimeConversion")
			{
				double time_conv = it1++->second.as<double>();
				this->SetTimeConversion(time_conv);
				continue;
			}
			if (keyword == "SetTimeStep")
			{
				double time_step = it1++->second.as<double>();
				this->SetTimeStep(time_step);
				continue;
			}
			if (keyword == "SetUnitsExchange")
			{
				int units = it1++->second.as<int>();
				this->SetUnitsExchange(units);
				continue;
			}
			if (keyword == "SetUnitsGasPhase")
			{
				int units = it1++->second.as< int >();
				this->SetUnitsGasPhase(units);
				continue;
			}
			if (keyword == "SetUnitsKinetics")
			{
				int units = it1++->second.as< int >();
				this->SetUnitsKinetics(units);
				continue;
			}
			if (keyword == "SetUnitsPPassemblage")
			{
				int units = it1++->second.as< int >();
				this->SetUnitsPPassemblage(units);
				continue;
			}
			if (keyword == "SetUnitsSolution")
			{
				int units = it1++->second.as< int >();
				this->SetUnitsSolution(units);
				continue;
			}
			if (keyword == "SetUnitsSSassemblage")
			{
				int units = it1++->second.as< int >();
				this->SetUnitsSSassemblage(units);
				continue;
			}
			if (keyword == "SetUnitsSurface")
			{
				int units = it1++->second.as< int >();
				this->SetUnitsSurface(units);
				continue;
			}
			if (keyword == "SpeciesConcentrations2Module")
			{
				std::vector < double > scond = it1++->second.as< std::vector < double > >();
				this->SpeciesConcentrations2Module(scond);
				continue;
			}
			if (keyword == "StateSave")
			{
				int n = it1++->second.as< int >();
				this->StateSave(n);
				continue;
			}
			if (keyword == "StateApply")
			{
				int n = it1++->second.as< int >();
				this->StateApply(n);
				continue;
			}
			if (keyword == "StateDelete")
			{
				int n = it1++->second.as< int >();
				this->StateDelete(n);
				continue;
			}
			if (keyword == "ThreadCount") {
				continue;
			}
			if (keyword == "UseSolutionDensityVolume")
			{
				bool tf = it1++->second.as< bool >();
				this->UseSolutionDensityVolume(tf);
				continue;
			}
			if (keyword == "WarningMessage")
			{
				std::string str = it1++->second.as< std::string >();
				this->WarningMessage(str);
				continue;
			}
			//throw LetItThrow("YAML keyword not found");
			std::ostringstream oss;
			oss << "YAML keyword not found: " << keyword << std::endl;
			ErrorMessage(oss.str());
			throw PhreeqcRMStop();
		}
	}
	return IRM_RESULT::IRM_OK;
}
#endif
/* ---------------------------------------------------------------------- */
IRM_RESULT
PhreeqcRM::InitialPhreeqc2Concentrations(std::vector < double > &destination_c,
					const std::vector < int > & boundary_solution1)
{
	this->phreeqcrm_error_string.clear();
	std::vector< int > dummy;
	std::vector< double > dummy1;
	return InitialPhreeqc2Concentrations(destination_c, boundary_solution1, dummy, dummy1);
}
/* ---------------------------------------------------------------------- */
IRM_RESULT
PhreeqcRM::InitialPhreeqc2Concentrations(std::vector < double > &destination_c,
					const std::vector < int > & boundary_solution1,
					const std::vector < int > & boundary_solution2,
					const std::vector < double > & fraction1)
/* ---------------------------------------------------------------------- */
{
/*
 *   Routine takes a list of solution numbers and returns a set of
 *   concentrations
 *   Input: n_boundary - number of boundary conditions in list
 *          boundary_solution1 - list of first solution numbers to be mixed
 *          boundary_solution2 - list of second solution numbers to be mixed
 *          fraction1 - fraction of first solution 0 <= f <= 1
 *
 *   Output: destination_c - concentrations for boundary conditions
 *
 */
	this->phreeqcrm_error_string.clear();
	IRM_RESULT return_value = IRM_OK;
	this->Get_phreeqc_bin().Clear();
	try
	{
		if (boundary_solution1.size() > 0)
		{
			destination_c.resize(this->components.size()*boundary_solution1.size());
			int	n_old1, n_old2;
			double f1, f2;
			size_t n_boundary1 = boundary_solution1.size();
			size_t n_boundary2 = boundary_solution2.size();
			size_t n_fraction1 = fraction1.size();
			for (size_t i = 0; i < n_boundary1; i++)
			{
				// Find solution 1 number
				n_old1 = boundary_solution1[i];
				if (n_old1 < 0)
				{
					int next = this->GetWorkers()[this->nthreads]->Get_PhreeqcPtr()->next_user_number(Keywords::KEY_SOLUTION);
					if (next != 0)
					{
						n_old1 = next - 1;
					}
					next = this->GetWorkers()[this->nthreads]->Get_PhreeqcPtr()->next_user_number(Keywords::KEY_MIX);
					if (next - 1 > n_old1)
						n_old1 = next -1;
				}

				// Put solution 1 in storage bin
				IRM_RESULT status = IRM_OK;
				if (this->Get_phreeqc_bin().Get_Solution(n_old1) == NULL)
				{
					if (n_old1 >= 0)
					{
						this->GetWorkers()[this->nthreads]->Get_PhreeqcPtr()->phreeqc2cxxStorageBin(this->Get_phreeqc_bin(), n_old1);
					}
					else
					{
						cxxSolution cxxsoln;
						this->Get_phreeqc_bin().Set_Solution(n_old1, cxxsoln);
					}
				}
				this->ErrorHandler(status, "First solution for InitialPhreeqc2Concentrations");

				f1 = 1.0;
				if (i < n_fraction1)
					f1 = fraction1[i];
				cxxMix mixmap;
				mixmap.Add(n_old1, f1);

				// Solution 2
				n_old2 = -1;
				if (i < n_boundary2)
				{
					n_old2 = boundary_solution2[i];
				}
				f2 = 1 - f1;
				status = IRM_OK;
				if (n_old2 >= 0 && f2 > 0.0)
				{
					if (this->Get_phreeqc_bin().Get_Solution(n_old2) == NULL)
					{
						this->GetWorkers()[this->nthreads]->Get_PhreeqcPtr()->phreeqc2cxxStorageBin(this->Get_phreeqc_bin(), n_old2);
					}
					mixmap.Add(n_old2, f2);
				}
				this->ErrorHandler(status, "Second solution for InitialPhreeqc2Concentrations");

				// Make concentrations in destination_c
				std::vector<double> d;
				cxxSolution	cxxsoln(phreeqc_bin->Get_Solutions(), mixmap, 0);
				double v = cxxsoln.Get_soln_vol();
				double dens = cxxsoln.Get_density();
				cxxSolution2concentration(&cxxsoln, d, v, dens);

				// Put concentrations in c
				double *d_ptr = &destination_c[i];
				for (size_t j = 0; j < components.size(); j++)
				{
					d_ptr[n_boundary1 * j] = d[j];
				}
			}
			return IRM_OK;
		}
		this->ErrorHandler(IRM_INVALIDARG, "NULL pointer or dimension of zero in arguments.");
	}
	catch (...)
	{
		return_value = IRM_FAIL;
	}
	return this->ReturnHandler(return_value, "PhreeqcRM::InitialPhreeqc2Concentrations");
}
/* ---------------------------------------------------------------------- */
IRM_RESULT
PhreeqcRM::InitialSolutions2Module(const std::vector < int >& solutions)
/* ---------------------------------------------------------------------- */
{
	this->phreeqcrm_error_string.clear();
	std::vector<int> i_dummy, ic;
	std::vector<double> d_dummy;
	if (mpi_myself == 0)
	{
		ic.resize(this->nxyz * 7, -1);
		i_dummy.resize(this->nxyz * 7, -1);
		d_dummy.resize(this->nxyz * 7, 1.0);
		for (size_t i = 0; i < this->nxyz; i++)
		{
			ic[i] = solutions[i];       
		}
	}
	return InitialPhreeqc2Module(ic, i_dummy, d_dummy);
}
/* ---------------------------------------------------------------------- */
IRM_RESULT
PhreeqcRM::InitialEquilibriumPhases2Module(const std::vector < int >& equilibrium_phases)
/* ---------------------------------------------------------------------- */
{
	this->phreeqcrm_error_string.clear();
	std::vector<int> i_dummy, ic;
	std::vector<double> d_dummy;
	if (mpi_myself == 0)
	{
		ic.resize((size_t)this->nxyz * 7, -1);
		i_dummy.resize((size_t)this->nxyz * 7, -1);
		d_dummy.resize((size_t)this->nxyz * 7, 1.0);
		for (size_t i = 0; i < this->nxyz; i++)
		{
			ic[(size_t)this->nxyz + i] = equilibrium_phases[i];
		}
	}
	return InitialPhreeqc2Module(ic, i_dummy, d_dummy);
}
/* ---------------------------------------------------------------------- */
IRM_RESULT
PhreeqcRM::InitialExchanges2Module(const std::vector < int >& exchanges)
/* ---------------------------------------------------------------------- */
{
	this->phreeqcrm_error_string.clear();
	std::vector<int> i_dummy, ic;
	std::vector<double> d_dummy;
	if (mpi_myself == 0)
	{
		ic.resize((size_t)this->nxyz * 7, -1);
		i_dummy.resize((size_t)this->nxyz * 7, -1);
		d_dummy.resize((size_t)this->nxyz * 7, 1.0);
		for (size_t i = 0; i < this->nxyz; i++)
		{
				ic[2 * (size_t)this->nxyz + i] = exchanges[i];      
		}
	}
	return InitialPhreeqc2Module(ic, i_dummy, d_dummy);
}
/* ---------------------------------------------------------------------- */
IRM_RESULT
PhreeqcRM::InitialSurfaces2Module(const std::vector < int >& surfaces)
/* ---------------------------------------------------------------------- */
{
	this->phreeqcrm_error_string.clear();
	std::vector<int> i_dummy, ic;
	std::vector<double> d_dummy;
	if (mpi_myself == 0)
	{
		ic.resize((size_t)this->nxyz * 7, -1);
		i_dummy.resize((size_t)this->nxyz * 7, -1);
		d_dummy.resize((size_t)this->nxyz * 7, 1.0);
		for (size_t i = 0; i < this->nxyz; i++)
		{
			ic[3 * (size_t)this->nxyz + i] = surfaces[i];
		}
	}
	return InitialPhreeqc2Module(ic, i_dummy, d_dummy);
}
/* ---------------------------------------------------------------------- */
IRM_RESULT
PhreeqcRM::InitialGasPhases2Module(const std::vector < int >& gas_phases)
/* ---------------------------------------------------------------------- */
{
	this->phreeqcrm_error_string.clear();
	std::vector<int> i_dummy, ic;
	std::vector<double> d_dummy;
	if (mpi_myself == 0)
	{
		ic.resize((size_t)this->nxyz * 7, -1);
		i_dummy.resize((size_t)this->nxyz * 7, -1);
		d_dummy.resize((size_t)this->nxyz * 7, 1.0);
		for (size_t i = 0; i < this->nxyz; i++)
		{
			ic[4 * (size_t)this->nxyz + i] = gas_phases[i];
		}
	}
	return InitialPhreeqc2Module(ic, i_dummy, d_dummy);
}
/* ---------------------------------------------------------------------- */
IRM_RESULT
PhreeqcRM::InitialSolidSolutions2Module(const std::vector < int >& solid_solutions)
/* ---------------------------------------------------------------------- */
{
	this->phreeqcrm_error_string.clear();
	std::vector<int> i_dummy, ic;
	std::vector<double> d_dummy;
	if (mpi_myself == 0)
	{
		ic.resize((size_t)this->nxyz * 7, -1);
		i_dummy.resize((size_t)this->nxyz * 7, -1);
		d_dummy.resize((size_t)this->nxyz * 7, 1.0);
		for (size_t i = 0; i < this->nxyz; i++)
		{
			ic[5 * (size_t)this->nxyz + i] = solid_solutions[i];
		}
	}
	return InitialPhreeqc2Module(ic, i_dummy, d_dummy);
}
/* ---------------------------------------------------------------------- */
IRM_RESULT
PhreeqcRM::InitialKinetics2Module(const std::vector < int >& kinetics)
/* ---------------------------------------------------------------------- */
{
	this->phreeqcrm_error_string.clear();
	std::vector<int> i_dummy, ic;
	std::vector<double> d_dummy;
	if (mpi_myself == 0)
	{
		ic.resize((size_t)this->nxyz * 7, -1);
		i_dummy.resize((size_t)this->nxyz * 7, -1);
		d_dummy.resize((size_t)this->nxyz * 7, 1.0);
		for (size_t i = 0; i < this->nxyz; i++)
		{
			ic[6 * (size_t)this->nxyz + i] = kinetics[i];
		}
	}
	return InitialPhreeqc2Module(ic, i_dummy, d_dummy);
}
/* ---------------------------------------------------------------------- */
IRM_RESULT
PhreeqcRM::InitialPhreeqc2Module(
					const std::vector < int >    & initial_conditions1_in)
/* ---------------------------------------------------------------------- */
{
	this->phreeqcrm_error_string.clear();
	std::vector<int> i_dummy;
	std::vector<double> d_dummy;
	if (mpi_myself == 0)
	{
		i_dummy.resize(this->nxyz*7, -1);
		d_dummy.resize(this->nxyz*7,1.0);
	}
	return InitialPhreeqc2Module(initial_conditions1_in, i_dummy, d_dummy);
}
/* ---------------------------------------------------------------------- */
IRM_RESULT
PhreeqcRM::InitialPhreeqc2Module(
					const std::vector < int >    & initial_conditions1,
					const std::vector < int >    & initial_conditions2,
					const std::vector < double > & fraction1_in)
/* ---------------------------------------------------------------------- */
{
	/*
	 *      nxyz - number of cells
	 *      initial_conditions1 - Fortran, 7 x nxyz integer array, containing
	 *      entity numbers for
	 *           solution number
	 *           pure_phases number
	 *           exchange number
	 *           surface number
	 *           gas number
	 *           solid solution number
	 *           kinetics number
	 *      initial_conditions2 - Fortran, 7 x nxyz integer array, containing
	 *			 entity numbers
	 *      fraction1 - Fortran 7 x n_cell  double array, fraction for entity 1
	 *
	 *      Routine mixes solutions, pure_phase assemblages,
	 *      exchangers, surface complexers, gases, solid solution assemblages,
	 *      and kinetics for each cell.
	 */
	this->phreeqcrm_error_string.clear();
	IRM_RESULT return_value = IRM_OK;
	try
	{
		std::vector<double> fraction1 = fraction1_in;
		if (mpi_myself == 0)
		{
			if ((int) initial_conditions1.size() != (7 * this->nxyz))
			{
				this->ErrorHandler(IRM_INVALIDARG, "initial_conditions1 vector is the wrong size in InitialPhreeqc2Module");
			}
			if ((int) initial_conditions2.size() > 0 && (int) initial_conditions2.size() != (7 * this->nxyz))
			{
				this->ErrorHandler(IRM_INVALIDARG, "initial_conditions2 vector is the wrong size in InitialPhreeqc2Module");
			}
			if ((int) fraction1.size() > 0 && (int) fraction1.size() != (7 * this->nxyz))
			{
				this->ErrorHandler(IRM_INVALIDARG, "fraction1 vector is the wrong size in InitialPhreeqc2Module");
			}
		}

		// Use phreeqc_bin to capture InitialPhreeqc definitions
		this->Get_phreeqc_bin().Clear();
		this->GetWorkers()[this->nthreads]->Get_PhreeqcPtr()->phreeqc2cxxStorageBin(this->Get_phreeqc_bin());
#ifdef USE_MPI
		if (this->mpi_myself == 0)
		{
			int method = METHOD_INITIALPHREEQC2MODULE;
			MPI_Bcast(&method, 1, MPI_INT, 0, phreeqcrm_comm);
		}
		// distribute initial conditions to workers
		std::vector<int> ic1, ic2, ic1_worker, ic2_worker, ic1_root, ic2_root;
		std::vector<double> f1, f1_worker, f1_root;
		ic1_root.resize(this->nxyz);
		ic2_root.resize(this->nxyz);
		f1_root.resize(this->nxyz);
		for (int i = 0; i < 7; i++)
		{
			// pull out next chunk
			if (mpi_myself == 0)
			{ 
				memcpy(&ic1_root.front(), &initial_conditions1[i * this->nxyz], (size_t) this->nxyz * sizeof(int));
				memcpy(&ic2_root.front(), &initial_conditions2[i * this->nxyz], (size_t) this->nxyz * sizeof(int));
				memcpy(&f1_root.front(), &fraction1[i * this->nxyz], (size_t) this->nxyz * sizeof(double));
			}
			ScatterNchem(ic1_root, ic1);
			ScatterNchem(ic2_root, ic2);
			ScatterNchem(f1_root, f1);
			//a.insert(a.end(), b.begin(), b.end());
			ic1_worker.insert(ic1_worker.end(), ic1.begin(), ic1.end());
			ic2_worker.insert(ic2_worker.end(), ic2.begin(), ic2.end());
			f1_worker.insert(f1_worker.end(), f1.begin(), f1.end());
		}
#endif
		/*
		*  Copy solution, exchange, surface, gas phase, kinetics, solid solution for each active cell.
		*  Does nothing for indexes less than 0 (i.e. restart files)
		*/

		size_t count_negative_porosity = 0;
		std::ostringstream errstr;

#ifdef USE_MPI
		int begin = this->start_cell[this->mpi_myself];
		int end = this->end_cell[this->mpi_myself] + 1;
#else
		int begin = 0;
		int end = this->count_chemistry;
#endif
		for (int i = begin; i < end; i++)  		    /* i is count_chem number */
		{
			std::set<std::string> error_set;
#ifdef USE_MPI
			int j = i; // not used
			int l = i - begin;
			double por = porosity_worker[l];
			double repv = rv_worker[l];
#else
			int j = this->backward_mapping[i][0];	/* j is nxyz number */
			if (j < 0)	continue;
			double por = porosity_root[j];
			double repv = rv_root[j];
#endif
			assert (por >= 0.0);
			if (por < 0.0)
			{
				errstr << "Nonpositive porosity in cell " << i << ": porosity, " << por;
				errstr <<  "." << std::endl;
				count_negative_porosity++;
				return_value = IRM_FAIL;
				continue;
			}
			assert (repv >= 0.0);
			if (repv < 0.0)
			{
				errstr << "Nonpositive representative volume in cell " << i << ": representative volume, " << repv;
				errstr <<  "." << std::endl;
				count_negative_porosity++;
				return_value = IRM_FAIL;
				continue;
			}
#ifdef USE_MPI
			if (this->CellInitialize(j, i, &ic1_worker.front(), &ic2_worker.front(),
				&f1_worker.front(), error_set) != IRM_OK)
#else
			if (this->CellInitialize(j, i, &initial_conditions1.front(), &initial_conditions2.front(),
				&fraction1.front(), error_set) != IRM_OK)
#endif
			{
				std::set<std::string>::iterator it = error_set.begin();
				for (; it != error_set.end(); it++)
				{
					errstr << it->c_str() << "\n";
				}
				return_value = IRM_FAIL;
			}
		}
		if (count_negative_porosity > 0)
		{
			return_value = IRM_FAIL;
			errstr << "Negative initial volumes may be due to initial head distribution.\n"
				"Make initial heads greater than or equal to the elevation of the node for each cell.\n"
				"Increase porosity, decrease specific storage, or use free surface boundary.";
		}
		if (return_value != IRM_OK)
		{
			//std::cerr << errstr.str() << std::endl;
			this->ErrorMessage(errstr.str());
		}
#ifdef USE_MPI
		std::vector<int> r_values;
		r_values.push_back(return_value);
		this->HandleErrorsInternal(r_values);
#else
		this->ErrorHandler(return_value, "Processing initial conditions.");
		// distribute to thread IPhreeqcs
		std::vector<int> r_values;
		r_values.resize(this->nthreads, 0);
		for (int n = 1; n < this->nthreads; n++)
		{
			std::ostringstream delete_command;
			delete_command << "DELETE; -cells\n";
			for (int i = this->start_cell[n]; i <= this->end_cell[n]; i++)
			{
				cxxStorageBin sz_bin;
				this->GetWorkers()[0]->Get_PhreeqcPtr()->phreeqc2cxxStorageBin(sz_bin, i);
				this->GetWorkers()[n]->Get_PhreeqcPtr()->cxxStorageBin2phreeqc(sz_bin, i);
				delete_command << i << "\n";
			}
			r_values[n] = this->GetWorkers()[0]->RunString(delete_command.str().c_str());
			if (r_values[n] != 0)
			{
				this->ErrorMessage(this->GetWorkers()[0]->GetErrorString());
			}
		}
		this->HandleErrorsInternal(r_values);
#endif
	}	
	catch(std::exception &e)
	{
		std::string errmsg("InitialPhreeqc2Module: ");
		errmsg += e.what();
		this->ErrorMessage(errmsg.c_str()); 
		return_value = IRM_FAIL;
	}
	catch (...)
	{
		return_value = IRM_FAIL;
	}
	return this->ReturnHandler(return_value, "PhreeqcRM::InitialPhreeqc2Module");
}
/* ---------------------------------------------------------------------- */
IRM_RESULT
PhreeqcRM::InitialPhreeqc2SpeciesConcentrations(std::vector < double > &destination_c,
					const std::vector < int > & boundary_solution1)
{
	this->phreeqcrm_error_string.clear();
	std::vector< int > dummy;
	std::vector< double > dummy1;
	return InitialPhreeqc2SpeciesConcentrations(destination_c, boundary_solution1, dummy, dummy1);
}

/* ---------------------------------------------------------------------- */
IRM_RESULT
PhreeqcRM::InitialPhreeqc2SpeciesConcentrations(std::vector < double > &destination_c,
					const std::vector < int > & boundary_solution1,
					const std::vector < int > & boundary_solution2,
					const std::vector < double > & fraction1)
/* ---------------------------------------------------------------------- */
{
/*
 *   Routine takes a list of solution numbers and returns a set of
 *   concentrations
 *   Input: n_boundary - number of boundary conditions in list
 *          boundary_solution1 - list of first solution numbers to be mixed
 *          boundary_solution2 - list of second solution numbers to be mixed
 *          fraction1 - fraction of first solution 0 <= f <= 1
 *
 *   Output: c - concentrations for boundary conditions
 *
 */
	this->phreeqcrm_error_string.clear();
	IRM_RESULT return_value = IRM_OK;
	this->Get_phreeqc_bin().Clear();
	try
	{
		if (boundary_solution1.size() > 0 && this->species_names.size() > 0)
		{
			destination_c.resize(this->species_names.size()*boundary_solution1.size(), 0.0);
			int	n_old1, n_old2;
			double f1, f2;
			size_t n_boundary1 = boundary_solution1.size();
			size_t n_boundary2 = boundary_solution2.size();
			size_t n_fraction1 = fraction1.size();
			for (size_t i = 0; i < n_boundary1; i++)
			{
				// Find solution 1 number
				n_old1 = boundary_solution1[i];
				if (n_old1 < 0)
				{
					int next = this->GetWorkers()[this->nthreads]->Get_PhreeqcPtr()->next_user_number(Keywords::KEY_SOLUTION);
					if (next != 0)
					{
						n_old1 = next - 1;
					}
					next = this->GetWorkers()[this->nthreads]->Get_PhreeqcPtr()->next_user_number(Keywords::KEY_MIX);
					if (next - 1 > n_old1)
						n_old1 = next -1;
				}

				// Put solution 1 in storage bin
				IRM_RESULT status = IRM_OK;
				if (this->Get_phreeqc_bin().Get_Solution(n_old1) == NULL)
				{
					if (n_old1 >= 0)
					{
						this->GetWorkers()[this->nthreads]->Get_PhreeqcPtr()->phreeqc2cxxStorageBin(this->Get_phreeqc_bin(), n_old1);
					}
					else
					{
						cxxSolution cxxsoln;
						this->Get_phreeqc_bin().Set_Solution(n_old1, cxxsoln);
					}
				}
				this->ErrorHandler(status, "First solution for InitialPhreeqc2Concentrations");

				f1 = 1.0;
				if (i < n_fraction1)
					f1 = fraction1[i];
				cxxMix mixmap;
				mixmap.Add(n_old1, f1);

				// Solution 2
				n_old2 = -1;
				if (i < n_boundary2)
				{
					n_old2 = boundary_solution2[i];
				}
				f2 = 1 - f1;
				status = IRM_OK;
				if (n_old2 >= 0 && f2 > 0.0)
				{
					if (this->Get_phreeqc_bin().Get_Solution(n_old2) == NULL)
					{
						this->GetWorkers()[this->nthreads]->Get_PhreeqcPtr()->phreeqc2cxxStorageBin(this->Get_phreeqc_bin(), n_old2);
					}
					mixmap.Add(n_old2, f2);
				}
				this->ErrorHandler(status, "Second solution for InitialPhreeqc2Concentrations");

				// Make concentrations in destination_c
				cxxSolution	cxxsoln(phreeqc_bin->Get_Solutions(), mixmap, 0);
				std::vector<double> d;
				d.resize(this->species_names.size(), 0);
				std::map<int, double>::iterator it = cxxsoln.Get_species_map().begin();
				for ( ; it != cxxsoln.Get_species_map().end(); it++)
				{
					int rm_species_num = this->s_num2rm_species_num[it->first];
					d[rm_species_num] = it->second;
				}

				// Put concentrations in c
				double *d_ptr = &destination_c[i];
				for (size_t j = 0; j < species_names.size(); j++)
				{
					d_ptr[n_boundary1 * j] = d[j];
				}
			}
			return IRM_OK;
		}
		this->ErrorHandler(IRM_INVALIDARG, "Size of boundary1 or species is zero.");
	}
	catch (...)
	{
		return_value = IRM_FAIL;
	}
	return this->ReturnHandler(return_value, "PhreeqcRM::InitialPhreeqc2SpeciesConcentrations");
}
#ifdef SKIP
/* ---------------------------------------------------------------------- */
IRM_RESULT
PhreeqcRM::InitialPhreeqcCell2Module(int cell, const std::vector<int> &cell_numbers_in)
/* ---------------------------------------------------------------------- */
{
	/*
	 *      Routine finds the last solution in InitialPhreeqc, equilibrates the cell,
	 *      and copies result to list of cell numbers in the module.
	 */
	this->phreeqcrm_error_string.clear();
#ifdef USE_MPI
	if (this->mpi_myself == 0)
	{
		int method = METHOD_INITIALPHREEQCCELL2MODULE;
		MPI_Bcast(&method, 1, MPI_INT, 0, phreeqcrm_comm);
	}
#endif
	IRM_RESULT return_value = IRM_OK;
	if (this->mpi_myself == 0)
	{
		// determine last solution number
		if (cell < 0)
		{
			int next = this->GetWorkers()[this->nthreads]->Get_PhreeqcPtr()->next_user_number(Keywords::KEY_SOLUTION);
			if (next != 0)
			{
				cell = next - 1;
			}
		}
		else
		{
			cxxSolution *soln = this->GetWorkers()[this->nthreads]->Get_solution(cell);
			if (soln == NULL)
				cell = -1;
		}
	}
#ifdef USE_MPI
	MPI_Bcast(&cell, 1, MPI_INT, 0, phreeqcrm_comm);
#endif
	// cell not found
	if (cell < 0)
	{
		return_value = IRM_INVALIDARG;
		return this->ReturnHandler(return_value, "PhreeqcRM::InitialPhreeqcCell2Module");
	}
	std::vector< int > cell_numbers;
	if (this->mpi_myself == 0)
	{
		cell_numbers = cell_numbers_in;
	}
	// transfer the cell to domain
#ifdef USE_MPI
	int n_cells;
	if (this->mpi_myself == 0)
	{
		n_cells = (int) cell_numbers.size();
	}
	MPI_Bcast(&n_cells, 1, MPI_INT, 0, phreeqcrm_comm);
	cell_numbers.resize(n_cells);
	MPI_Bcast((void *) &cell_numbers.front(), n_cells, MPI_INT, 0, phreeqcrm_comm);
#endif
	try
	{
		for (size_t i = 0; i < cell_numbers.size(); i++)
		{
			int nchem = this->forward_mapping[cell_numbers[i]];
			if (nchem < 0 || nchem >= this->count_chemistry) continue;
#ifdef USE_MPI
			int n = this->mpi_myself;
			if (nchem >= start_cell[n] && nchem <= end_cell[n])
			{
				{
#else
			for (int n = 0; n < nthreads; n++)
			{
				if (nchem >= start_cell[n] && nchem <= end_cell[n])
				{
#endif
					cxxStorageBin cell_bin;
					this->GetWorkers()[this->nthreads]->Get_PhreeqcPtr()->phreeqc2cxxStorageBin(cell_bin, cell);
					cell_bin.Remove_Mix(cell);
					cell_bin.Remove_Reaction(cell);
					cell_bin.Remove_Temperature(cell);
					cell_bin.Remove_Pressure(cell);
#ifdef USE_MPI
					int l = nchem - start_cell[n];
					double cell_porosity_local = this->porosity_worker[l];
					double cell_rv_local = this->rv_worker[l];
					double cell_saturation_local = this->saturation_worker[l];
#else
					double cell_porosity_local = this->porosity_root[cell_numbers[i]];
					double cell_rv_local = this->rv_root[cell_numbers[i]];
					double cell_saturation_local = this->saturation_root[cell_numbers[i]];
#endif
					// solution
					{
						cxxMix mx;
						double current_v = cell_bin.Get_Solution(cell)->Get_soln_vol();
						double v = cell_porosity_local * cell_saturation_local * cell_rv_local / current_v;
						mx.Add((int) cell, v);
						cxxSolution cxxsoln(cell_bin.Get_Solutions(), mx, nchem);
						cell_bin.Set_Solution(nchem, &cxxsoln);
					}

					// for solids
					std::vector < double > porosity_factor;
					porosity_factor.push_back(cell_rv_local);                              // per liter of rv
					porosity_factor.push_back(cell_rv_local*cell_porosity_local);          // per liter of water
					porosity_factor.push_back(cell_rv_local*(1.0 - cell_porosity_local));  // per liter of rock

					// pp_assemblage
					if (cell_bin.Get_PPassemblages().find(cell) != cell_bin.Get_PPassemblages().end())
					{
						cxxMix mx;
						mx.Add(cell, porosity_factor[this->units_PPassemblage]);
						cxxPPassemblage cxxentity(cell_bin.Get_PPassemblages(), mx, nchem);
						cell_bin.Set_PPassemblage(nchem, &cxxentity);
					}
					// exchange
					if (cell_bin.Get_Exchangers().find(cell) != cell_bin.Get_Exchangers().end())
					{
						cxxMix mx;
						mx.Add(cell, porosity_factor[this->units_Exchange]);
						cxxExchange cxxentity(cell_bin.Get_Exchangers(), mx, nchem);
						cell_bin.Set_Exchange(nchem, &cxxentity);
					}
					// surface assemblage
					if (cell_bin.Get_Surfaces().find(cell) != cell_bin.Get_Surfaces().end())
					{
						cxxMix mx;
						mx.Add(cell, porosity_factor[this->units_Surface]);
						cxxSurface cxxentity(cell_bin.Get_Surfaces(), mx, nchem);
						cell_bin.Set_Surface(nchem, &cxxentity);
					}
					// gas phase
					if (cell_bin.Get_GasPhases().find(cell) != cell_bin.Get_GasPhases().end())
					{
						cxxMix mx;
						mx.Add(cell, porosity_factor[this->units_GasPhase]);
						cxxGasPhase cxxentity(cell_bin.Get_GasPhases(), mx, nchem);
						cell_bin.Set_GasPhase(nchem, &cxxentity);
					}
					// solid solution
					if (cell_bin.Get_SSassemblages().find(cell) != cell_bin.Get_SSassemblages().end())
					{
						cxxMix mx;
						mx.Add(cell, porosity_factor[this->units_SSassemblage]);
						cxxSSassemblage cxxentity(cell_bin.Get_SSassemblages(), mx, nchem);
						cell_bin.Set_SSassemblage(nchem, &cxxentity);
					}
					// solid solution
					if (cell_bin.Get_Kinetics().find(cell) != cell_bin.Get_Kinetics().end())
					{
						cxxMix mx;
						mx.Add(cell, porosity_factor[this->units_Kinetics]);
						cxxKinetics cxxentity(cell_bin.Get_Kinetics(), mx, nchem);
						cell_bin.Set_Kinetics(nchem, &cxxentity);
					}
#ifdef USE_MPI
					this->GetWorkers()[0]->Get_PhreeqcPtr()->cxxStorageBin2phreeqc(cell_bin, nchem);
#else
					this->GetWorkers()[n]->Get_PhreeqcPtr()->cxxStorageBin2phreeqc(cell_bin, nchem);
#endif
				}
			}
		}
	}
	catch (...)
	{
		return_value = IRM_FAIL;
	}
	return this->ReturnHandler(return_value, "PhreeqcRM::InitialPhreeqcCell2Module");
}
#endif
/* ---------------------------------------------------------------------- */
IRM_RESULT
PhreeqcRM::InitialPhreeqcCell2Module(int cell, const std::vector<int> &cell_numbers_in)
/* ---------------------------------------------------------------------- */
{
	/*
	 *      Routine finds the last solution in InitialPhreeqc, equilibrates the cell,
	 *      and copies result to list of cell numbers in the module.
	 */
	this->phreeqcrm_error_string.clear();
#ifdef USE_MPI
	if (this->mpi_myself == 0)
	{
		int method = METHOD_INITIALPHREEQCCELL2MODULE;
		MPI_Bcast(&method, 1, MPI_INT, 0, phreeqcrm_comm);
	}
#endif
	IRM_RESULT return_value = IRM_OK;
	if (this->mpi_myself == 0)
	{
		// determine last solution number
		if (cell < 0)
		{
			int next = this->GetWorkers()[this->nthreads]->Get_PhreeqcPtr()->next_user_number(Keywords::KEY_SOLUTION);
			if (next != 0)
			{
				cell = next - 1;
			}
		}
		else
		{
			cxxSolution *soln = this->GetWorkers()[this->nthreads]->Get_solution(cell);
			if (soln == NULL)
				cell = -1;
		}
	}
#ifdef USE_MPI
	MPI_Bcast(&cell, 1, MPI_INT, 0, phreeqcrm_comm);
#endif
	// cell not found
	if (cell < 0)
	{
		return_value = IRM_INVALIDARG;
		return this->ReturnHandler(return_value, "PhreeqcRM::InitialPhreeqcCell2Module");
	}
	std::vector< int > cell_numbers;
	if (this->mpi_myself == 0)
	{
		//cell_numbers = cell_numbers_in;
		for (size_t i = 0; i < cell_numbers_in.size(); i++)
		{
			// cell numbers are in count_chemistry numbering
			cell_numbers.push_back(this->forward_mapping_root[cell_numbers_in[i]]);
		}
	}
	// transfer the cell to domain
#ifdef USE_MPI
	int n_cells = 0;
	if (this->mpi_myself == 0)
	{
		n_cells = (int) cell_numbers.size();
	}
	MPI_Bcast(&n_cells, 1, MPI_INT, 0, phreeqcrm_comm);
	cell_numbers.resize(n_cells);
	MPI_Bcast((void *) &cell_numbers.front(), n_cells, MPI_INT, 0, phreeqcrm_comm);
#endif
	try
	{
		for (size_t i = 0; i < cell_numbers.size(); i++)
		{
			//int nchem = this->forward_mapping[cell_numbers[i]];
			int nchem = cell_numbers[i];
			if (nchem < 0 || nchem >= this->count_chemistry) continue;
#ifdef USE_MPI
			int n = this->mpi_myself;
			if (nchem >= start_cell[n] && nchem <= end_cell[n])
			{
				{
#else
			for (int n = 0; n < nthreads; n++)
			{
				if (nchem >= start_cell[n] && nchem <= end_cell[n])
				{
#endif
					cxxStorageBin cell_bin;
					this->GetWorkers()[this->nthreads]->Get_PhreeqcPtr()->phreeqc2cxxStorageBin(cell_bin, cell);
					cell_bin.Remove_Mix(cell);
					cell_bin.Remove_Reaction(cell);
					cell_bin.Remove_Temperature(cell);
					cell_bin.Remove_Pressure(cell);
#ifdef USE_MPI
					int l = nchem - start_cell[n];
					double cell_porosity_local = this->porosity_worker[l];
					double cell_rv_local = this->rv_worker[l];
					double cell_saturation_local = this->saturation_worker[l];
#else
					double cell_porosity_local = this->porosity_root[cell_numbers_in[i]];
					double cell_rv_local = this->rv_root[cell_numbers_in[i]];
					double cell_saturation_local = this->saturation_root[cell_numbers_in[i]];
#endif
					// solution
					{
						cxxMix mx;
						double current_v = cell_bin.Get_Solution(cell)->Get_soln_vol();
						double v = cell_porosity_local * cell_saturation_local * cell_rv_local / current_v;
						mx.Add((int) cell, v);
						cxxSolution cxxsoln(cell_bin.Get_Solutions(), mx, nchem);
						cell_bin.Set_Solution(nchem, &cxxsoln);
					}

					// for solids
					std::vector < double > porosity_factor;
					porosity_factor.push_back(cell_rv_local);                              // per liter of rv
					porosity_factor.push_back(cell_rv_local*cell_porosity_local);          // per liter of water
					porosity_factor.push_back(cell_rv_local*(1.0 - cell_porosity_local));  // per liter of rock

					// pp_assemblage
					if (cell_bin.Get_PPassemblages().find(cell) != cell_bin.Get_PPassemblages().end())
					{
						cxxMix mx;
						mx.Add(cell, porosity_factor[this->units_PPassemblage]);
						cxxPPassemblage cxxentity(cell_bin.Get_PPassemblages(), mx, nchem);
						cell_bin.Set_PPassemblage(nchem, &cxxentity);
					}
					// exchange
					if (cell_bin.Get_Exchangers().find(cell) != cell_bin.Get_Exchangers().end())
					{
						cxxMix mx;
						mx.Add(cell, porosity_factor[this->units_Exchange]);
						cxxExchange cxxentity(cell_bin.Get_Exchangers(), mx, nchem);
						cell_bin.Set_Exchange(nchem, &cxxentity);
					}
					// surface assemblage
					if (cell_bin.Get_Surfaces().find(cell) != cell_bin.Get_Surfaces().end())
					{
						cxxMix mx;
						mx.Add(cell, porosity_factor[this->units_Surface]);
						cxxSurface cxxentity(cell_bin.Get_Surfaces(), mx, nchem);
						cell_bin.Set_Surface(nchem, &cxxentity);
					}
					// gas phase
					if (cell_bin.Get_GasPhases().find(cell) != cell_bin.Get_GasPhases().end())
					{
						cxxMix mx;
						mx.Add(cell, porosity_factor[this->units_GasPhase]);
						cxxGasPhase cxxentity(cell_bin.Get_GasPhases(), mx, nchem);
						cell_bin.Set_GasPhase(nchem, &cxxentity);
					}
					// solid solution
					if (cell_bin.Get_SSassemblages().find(cell) != cell_bin.Get_SSassemblages().end())
					{
						cxxMix mx;
						mx.Add(cell, porosity_factor[this->units_SSassemblage]);
						cxxSSassemblage cxxentity(cell_bin.Get_SSassemblages(), mx, nchem);
						cell_bin.Set_SSassemblage(nchem, &cxxentity);
					}
					// solid solution
					if (cell_bin.Get_Kinetics().find(cell) != cell_bin.Get_Kinetics().end())
					{
						cxxMix mx;
						mx.Add(cell, porosity_factor[this->units_Kinetics]);
						cxxKinetics cxxentity(cell_bin.Get_Kinetics(), mx, nchem);
						cell_bin.Set_Kinetics(nchem, &cxxentity);
					}
#ifdef USE_MPI
					this->GetWorkers()[0]->Get_PhreeqcPtr()->cxxStorageBin2phreeqc(cell_bin, nchem);
#else
					this->GetWorkers()[n]->Get_PhreeqcPtr()->cxxStorageBin2phreeqc(cell_bin, nchem);
#endif
				}
			}
		}
	}
	catch (...)
	{
		return_value = IRM_FAIL;
	}
	return this->ReturnHandler(return_value, "PhreeqcRM::InitialPhreeqcCell2Module");
}
/* ---------------------------------------------------------------------- */
IRM_RESULT
PhreeqcRM::Int2IrmResult(int i, bool positive_ok)
/* ---------------------------------------------------------------------- */
{
	IRM_RESULT return_value = IRM_OK;
	if (i < 0)
	{
		switch(i)
		{
		case IRM_OUTOFMEMORY:
			return_value = IRM_OUTOFMEMORY;
			break;
		case IRM_BADVARTYPE:
			return_value = IRM_BADVARTYPE;
			break;
		case IRM_INVALIDARG:
			return_value = IRM_INVALIDARG;
			break;
		case IRM_INVALIDROW:
			return_value = IRM_INVALIDROW;
			break;
		case IRM_INVALIDCOL:
			return_value = IRM_INVALIDCOL;
			break;
		case IRM_BADINSTANCE:
			return_value = IRM_BADINSTANCE;
			break;
		default:
			return_value = IRM_FAIL;
			break;
		}
	}
	if (!positive_ok && i > 0)
		return_value = IRM_FAIL;
	return return_value;
}

/* ---------------------------------------------------------------------- */
IRM_RESULT
PhreeqcRM::LoadDatabase(const std::string& database)
/* ---------------------------------------------------------------------- */
{
	this->phreeqcrm_error_string.clear();
#ifdef USE_MPI
	if (this->mpi_myself == 0)
	{
		int method = METHOD_LOADDATABASE;
		MPI_Bcast(&method, 1, MPI_INT, 0, phreeqcrm_comm);
	}
#endif
	IRM_RESULT return_value = IRM_OK;
	try
	{
		std::vector <int> r_vector;
		r_vector.resize(1);

		r_vector[0] = this->SetDatabaseFileName(database.c_str());
		this->HandleErrorsInternal(r_vector);

		// vector for return values
		r_vector.resize(this->nthreads + 2);

		// Load database for all IPhreeqc instances
#ifdef USE_OPENMP
		omp_set_num_threads(this->nthreads);
#pragma omp parallel
#pragma omp for
#endif
		for (int n = 0; n < this->nthreads; n++)
		{
			r_vector[n] = this->workers[n]->LoadDatabase(this->database_file_name.c_str());
		}
		for (int n = this->nthreads; n < this->nthreads + 2; n++)
		{
			r_vector[n] = this->workers[n]->LoadDatabase(this->database_file_name.c_str());
		}

		// Check errors
		this->HandleErrorsInternal(r_vector);
	}
	catch (...)
	{
		return_value = IRM_FAIL;
	}
#ifdef USE_MPI
	IRM_RESULT global_return_value;
	MPI_Allreduce(&return_value, &global_return_value, 1, MPI_INT, MPI_MIN, phreeqcrm_comm);
	return_value = global_return_value;
#endif
	for (int i = 0; i < this->nthreads + 1; i++)
	{
		this->workers[i]->PhreeqcPtr->save_species = this->species_save_on;
	}
	//if (var_man != NULL)
	//{
	//	this->RunString(false, true, false, "SOLUTION 1");
	//	std::vector<int> init(nxyz, 1);
	//	this->InitialSolutions2Module(init);
	//	var_man->NeedInitialRun = true;
	//	this->FindComponents();
	//	var_man->NeedInitialRun = false;
	//	//this->RunString(false, true, false, "DELETE; -all");
	//}
	return this->ReturnHandler(return_value, "PhreeqcRM::LoadDatabase");
}
/* ---------------------------------------------------------------------- */
void
PhreeqcRM::LogMessage(const std::string &str)
/* ---------------------------------------------------------------------- */
{
	this->phreeqcrm_io->log_msg(str.c_str());
}
/* ---------------------------------------------------------------------- */
int
PhreeqcRM::MpiAbort()
{
#ifdef USE_MPI
	return MPI_Abort(phreeqcrm_comm, 4);
#else
	return 0;
#endif
}
/* ---------------------------------------------------------------------- */
IRM_RESULT
PhreeqcRM::MpiWorker()
/* ---------------------------------------------------------------------- */
{
	// Called by all workers
	IRM_RESULT return_value = IRM_OK;
#ifdef USE_MPI
	this->worker_waiting = true;
	bool debug_worker = false;
	bool loop_break = false;
	while (!loop_break)
	{
		try
		{
			return_value = IRM_OK;
			int method = -1;
			//std::cerr << "Worker waiting..." << std::endl;
			MPI_Bcast(&method, 1, MPI_INT, 0, phreeqcrm_comm);
			switch (method)
			{
			case METHOD_CONSTRUCT:
				if (debug_worker) std::cerr << "METHOD_CONSTRUCT" << std::endl;
				{
					this->Construct();
				}
				break;
			case METHOD_CREATEMAPPING:
				if (debug_worker) std::cerr << "METHOD_CREATEMAPPING" << std::endl;
				{
					std::vector<int> dummy;
					return_value = this->CreateMapping(dummy);
				}
				break;
			case METHOD_DUMPMODULE:
				if (debug_worker) std::cerr << "METHOD_DUMPMODULE" << std::endl;
				{
					bool dummy = false;
					return_value = this->DumpModule(dummy);
				}
				break;
			case METHOD_FINDCOMPONENTS:
				if (debug_worker) std::cerr << "METHOD_FINDCOMPONENTS" << std::endl;
				this->FindComponents();
				break;
			case METHOD_GETCONCENTRATIONS:
				if (debug_worker) std::cerr << "METHOD_GETCONCENTRATIONS" << std::endl;
				{
					std::vector<double> dummy;
					return_value = this->GetConcentrations(dummy);
				}
				break;
			case METHOD_GETDENSITYCALCULATED:
				if (debug_worker) std::cerr << "METHOD_GETDENSITYCALCULATED" << std::endl;
				{
					std::vector<double> dummy;
					this->GetDensityCalculated(dummy);
				}
				break;
			case METHOD_GETERRORSTRING:
				if (debug_worker) std::cerr << "METHOD_GETERRORSTRING" << std::endl;
				{
					this->GetErrorString();
				}
				break;
			case METHOD_GETGASCOMPMOLES:
				if (debug_worker) std::cerr << "METHOD_GETGASCOMPMOLES" << std::endl;
				{
					std::vector<double> dummy;
					this->GetGasCompMoles(dummy);
				}
				break;
			case METHOD_GETGASCOMPPRESSURES:
				if (debug_worker) std::cerr << "METHOD_GETGASCOMPPRESSURES" << std::endl;
				{
					std::vector<double> dummy;
					this->GetGasCompPressures(dummy);
				}
				break;
			case METHOD_GETGASCOMPPHI:
				if (debug_worker) std::cerr << "METHOD_GETGASCOMPPHI" << std::endl;
				{
					std::vector<double> dummy;
					this->GetGasCompPhi(dummy);
				}
				break;
			case METHOD_GETGASPHASEVOLUME:
				if (debug_worker) std::cerr << "METHOD_GETGASPHASEVOLUME" << std::endl;
				{
					std::vector<double> dummy;
					this->GetGasPhaseVolume(dummy);
				}
				break;
			case METHOD_GETPRESSURE:
				if (debug_worker) std::cerr << "METHOD_GETPRESSURE" << std::endl;
				{
					this->GetPressure();
				}
				break;
			case METHOD_GETSATURATIONCALCULATED:
				if (debug_worker) std::cerr << "METHOD_GETSATURATIONCALCULATED" << std::endl;
				{
					std::vector<double> dummy;
					this->GetSaturationCalculated(dummy);
				}
				break;
			case METHOD_GETSELECTEDOUTPUT:
				if (debug_worker) std::cerr << "METHOD_GETSELECTEDOUTPUT" << std::endl;
				{
					std::vector<double> dummy;
					return_value = this->GetSelectedOutput(dummy);
				}
				break;
			case METHOD_GETSOLUTIONVOLUME:
				if (debug_worker) std::cerr << "METHOD_GETSOLUTIONVOLUME" << std::endl;
				{
					const std::vector<double> dummy = this->GetSolutionVolume();
				}
				break;
			case METHOD_GETSPECIESCONCENTRATIONS:
				if (debug_worker) std::cerr << "METHOD_GETSPECIESCONCENTRATIONS" << std::endl;
				{
					std::vector<double> c;
					this->GetSpeciesConcentrations(c);
				}
				break;
			case METHOD_GETSPECIESLOG10GAMMAS:
				if (debug_worker) std::cerr << "METHOD_GETSPECIESLOG10GAMMAS" << std::endl;
				{
					std::vector<double> c;
					this->GetSpeciesLog10Gammas(c);
				}
				break;
			case METHOD_GETSPECIESLOG10MOLALITIES:
				if (debug_worker) std::cerr << "METHOD_GETSPECIESLOG10MOLALITIESS" << std::endl;
				{
					std::vector<double> c;
					this->GetSpeciesLog10Molalities(c);
				}
				break;
			case METHOD_GETTEMPERATURE:
				if (debug_worker) std::cerr << "METHOD_GETTEMPERATURE" << std::endl;
				{
					this->GetTemperature();
				}
				break;
			case METHOD_GETVISCOSITY:
				if (debug_worker) std::cerr << "METHOD_GETVISCOSITY" << std::endl;
				{
					this->GetViscosity();
				}
				break;
			case METHOD_INITIALPHREEQC2MODULE:
				if (debug_worker) std::cerr << "METHOD_INITIALPHREEQC2MODULE" << std::endl;
				{
					std::vector<int> ic1, ic2;
					std::vector<double> f1;
					return_value = this->InitialPhreeqc2Module(ic1, ic2, f1);
				}
				break;
			case METHOD_INITIALPHREEQCCELL2MODULE:
				{
					std::vector<int> dummy;
					if (debug_worker) std::cerr << "METHOD_INITIALPHREEQCCELL2MODULE" << std::endl;
					return_value = this->InitialPhreeqcCell2Module(-1, dummy);
				}
				break;
			case METHOD_LOADDATABASE:
				if (debug_worker) std::cerr << "METHOD_LOADDATABASE" << std::endl;
				{
					char dummy[2]=" ";
					return_value = this->LoadDatabase(dummy);
				}
				break;
			case METHOD_MPIWORKERBREAK:
				if (debug_worker) std::cerr << "METHOD_MPIWORKERBREAK" << std::endl;
				loop_break = true;
				break;
			case METHOD_RUNCELLS:
				if (debug_worker) std::cerr << "METHOD_RUNCELLS" << std::endl;
				return_value = this->RunCells();
				break;
			case METHOD_RUNFILE:
				if (debug_worker) std::cerr << "METHOD_RUNFILE" << std::endl;
				{
					bool dummy = false;
					char c_dummy[2]=" ";
					return_value = this->RunFile(dummy, dummy, dummy, c_dummy);
				}
				break;
			case METHOD_RUNSTRING:
				if (debug_worker) std::cerr << "METHOD_RUNSTRING" << std::endl;
				{
					bool dummy = false;
					char c_dummy[2]=" ";
					return_value = this->RunString(dummy, dummy, dummy, c_dummy);
				}
				break;
			case METHOD_SETCOMPONENTH2O:
				if (debug_worker) std::cerr << "METHOD_SETCOMPONENTH2O" << std::endl;
				{
					bool dummy = false;
					this->SetComponentH2O(dummy);
				}
				break;
			case METHOD_SETCONCENTRATIONS:
				if (debug_worker) std::cerr << "METHOD_SETCONCENTRATIONS" << std::endl;
				{
					std::vector<double> dummy;
					this->SetConcentrations(dummy);
				}
				break;
			case METHOD_SETDENSITYUSER:
				if (debug_worker) std::cerr << "METHOD_SETDENSITYUSER" << std::endl;
				{
					std::vector<double> dummy;
					this->SetDensityUser(dummy);
				}
				break;
			case METHOD_SETERRORHANDLERMODE:
				if (debug_worker) std::cerr << "METHOD_SETERRORHANDLERMODE" << std::endl;
				{
					int dummy = 1;
					return_value = this->SetErrorHandlerMode(dummy);
				}
				break;
			case METHOD_SETFILEPREFIX:
				if (debug_worker) std::cerr << "METHOD_SETFILEPREFIX" << std::endl;
				{
					char c_dummy[2]=" ";
					return_value = this->SetFilePrefix(c_dummy);
				}
				break;			
			case METHOD_SETGASCOMPMOLES:
					if (debug_worker) std::cerr << "METHOD_SETGASCOMPMOLES" << std::endl;
					{
						std::vector<double> dummy;
						return_value = this->SetGasCompMoles(dummy);
					}
					break;
			case METHOD_SETGASPHASEVOLUME:
				if (debug_worker) std::cerr << "METHOD_SETGASPHASEVOLUME" << std::endl;
				{
					std::vector<double> dummy;
					return_value = this->SetGasPhaseVolume(dummy);
				}
				break;
			case METHOD_SETPARTITIONUZSOLIDS:
				if (debug_worker) std::cerr << "METHOD_SETPARTITIONUZSOLIDS" << std::endl;
				{
					bool dummy = false;
					return_value = this->SetPartitionUZSolids(dummy);
				}
				break;
			case METHOD_SETPOROSITY:
				if (debug_worker) std::cerr << "METHOD_SETPOROSITY" << std::endl;
				{
					std::vector<double> dummy;
					this->SetPorosity(dummy);
				}
				break;
			case METHOD_SETPRESSURE:
				if (debug_worker) std::cerr << "METHOD_SETPRESSURE" << std::endl;
				{
					std::vector<double> dummy;
					this->SetPressure(dummy);
				}
				break;
			case METHOD_SETPRINTCHEMISTRYON:
				if (debug_worker) std::cerr << "METHOD_SETPRINTCHEMISTRYON" << std::endl;
				{
					bool dummy = false;
					return_value = this->SetPrintChemistryOn(dummy, dummy, dummy);
				}
				break;
			case METHOD_SETPRINTCHEMISTRYMASK:
				if (debug_worker) std::cerr << "METHOD_SETPRINTCHEMISTRYMASK" << std::endl;
				{
					std::vector<int> dummy;
					this->SetPrintChemistryMask(dummy);
				}
				break;
			case METHOD_SETREBALANCEBYCELL:
				if (debug_worker) std::cerr << "METHOD_SETREBALANCEBYCELL" << std::endl;
				{
					bool dummy = false;
					return_value = this->SetRebalanceByCell(dummy);
				}
				break;
			case METHOD_SETREBALANCEFRACTION:
				if (debug_worker) std::cerr << "METHOD_SETREBALANCEFRACTION" << std::endl;
				{
					double dummy = 0.0;
					return_value = this->SetRebalanceFraction(dummy);
				}
				break;
			case METHOD_SETREPRESENTATIVEVOLUME:
				if (debug_worker) std::cerr << "METHOD_SETREPRESENTATIVEVOLUME" << std::endl;
				{
					std::vector<double> dummy;
					this->SetRepresentativeVolume(dummy);
				}
				break;
			case METHOD_SETSATURATIONUSER:
				if (debug_worker) std::cerr << "METHOD_SETSATURATIONUSER" << std::endl;
				{
					std::vector<double> dummy;
					this->SetSaturationUser(dummy);
				}
				break;
			case METHOD_SETSELECTEDOUTPUTON:
				if (debug_worker) std::cerr << "METHOD_SETSELECTEDOUTPUTON" << std::endl;
				{
					bool dummy = false;
					return_value = this->SetSelectedOutputOn(dummy);
				}
				break;
			case METHOD_SETSPECIESSAVEON:
				if (debug_worker) std::cerr << "METHOD_SETSPECIESSAVEON" << std::endl;
				{
					bool t = true;
					return_value = this->SetSpeciesSaveOn(t);
				}
				break;
			case METHOD_SETTEMPERATURE:
				if (debug_worker) std::cerr << "METHOD_SETTEMPERATURE" << std::endl;
				{
					std::vector<double> dummy;
					this->SetTemperature(dummy);
				}
				break;
			case METHOD_SETTIME:
				if (debug_worker) std::cerr << "METHOD_SETTIME" << std::endl;
				{
					double dummy = 0;
					return_value = this->SetTime(dummy);
				}
				break;
			case METHOD_SETTIMECONVERSION:
				if (debug_worker) std::cerr << "METHOD_SETTIMECONVERSION" << std::endl;
				{
					double dummy = 0;
					return_value = this->SetTimeConversion(dummy);
				}
				break;
			case METHOD_SETTIMESTEP:
				if (debug_worker) std::cerr << "METHOD_SETTIMESTEP" << std::endl;
				{
					double dummy = 0;
					return_value = this->SetTimeStep(dummy);
				}
				break;
			case METHOD_SETUNITSEXCHANGE:
				if (debug_worker) std::cerr << "METHOD_SETUNITSEXCHANGE" << std::endl;
				{
					int dummy = 0;
					return_value = this->SetUnitsExchange(dummy);
				}
				break;
			case METHOD_SETUNITSGASPHASE:
				if (debug_worker) std::cerr << "METHOD_SETUNITSGASPHASE" << std::endl;
				{
					int dummy = 0;
					return_value = this->SetUnitsGasPhase(dummy);
				}
				break;
			case METHOD_SETUNITSKINETICS:
				if (debug_worker) std::cerr << "METHOD_SETUNITSKINETICS" << std::endl;
				{
					int dummy = 0;
					return_value = this->SetUnitsKinetics(dummy);
				}
				break;
			case METHOD_SETUNITSPPASSEMBLAGE:
				if (debug_worker) std::cerr << "METHOD_SETUNITSPPASSEMBLAGE" << std::endl;
				{
					int dummy = 0;
					return_value = this->SetUnitsPPassemblage(dummy);
				}
				break;
			case METHOD_SETUNITSSOLUTION:
				if (debug_worker) std::cerr << "METHOD_SETUNITSSOLUTION" << std::endl;
				{
					int dummy = 0;
					return_value = this->SetUnitsSolution(dummy);
				}
				break;
			case METHOD_SETUNITSSSASSEMBLAGE:
				if (debug_worker) std::cerr << "METHOD_SETUNITSSSASSEMBLAGE" << std::endl;
				{
					int dummy = 0;
					return_value = this->SetUnitsSSassemblage(dummy);
				}
				break;
			case METHOD_SETUNITSSURFACE:
				if (debug_worker) std::cerr << "METHOD_SETUNITSSURFACE" << std::endl;
				{
					int dummy = 0;
					return_value = this->SetUnitsSurface(dummy);
				}
				break;
			case METHOD_SPECIESCONCENTRATIONS2MODULE:
				if (debug_worker) std::cerr << "METHOD_SPECIESCONCENTRATIONS2MODULE" << std::endl;
				{
					std::vector<double> c;
					return_value = this->SpeciesConcentrations2Module(c);
				}
				break;
			case METHOD_STATESAVE:
				if (debug_worker) std::cerr << "METHOD_STATESAVE" << std::endl;
				{
					int idummy = 0;
					return_value = this->StateSave(idummy);
				}
				break;
			case METHOD_STATEAPPLY:
				if (debug_worker) std::cerr << "METHOD_STATEAPPLY" << std::endl;
				{
					int idummy = 0;
					return_value = this->StateApply(idummy);
				}
				break;
			case METHOD_STATEDELETE:
					if (debug_worker) std::cerr << "METHOD_STATEDELETE" << std::endl;
					{
						int idummy = 0;
						return_value = this->StateDelete(idummy);
					}
					break;
			case METHOD_USESOLUTIONDENSITYVOLUME:
				if (debug_worker) std::cerr << "METHOD_USESOLUTIONDENSITYVOLUME" << std::endl;
				{
					bool tf = true;
					this->UseSolutionDensityVolume(tf);
				}
				break;
			default:
				if (debug_worker) std::cerr << "default " << method << std::endl;
				if (this->mpi_worker_callback_fortran)
				{
					int return_int = mpi_worker_callback_fortran(&method);
					if (return_int != 0)
					{
						return_value = IRM_FAIL;
					}
				}
				if (this->mpi_worker_callback_c)
				{
					int return_int = mpi_worker_callback_c(&method, this->mpi_worker_callback_cookie);
					if (return_int != 0)
					{
						return_value = IRM_FAIL;
					}
				}
				break;
			}
			this->ErrorHandler(return_value, "Task returned error in MpiWorker.");
		}
		catch (...)
		{
			return_value = IRM_FAIL;
		}
	}
#endif
	return this->ReturnHandler(return_value, "PhreeqcRM::MpiWorker");
}
/* ---------------------------------------------------------------------- */
IRM_RESULT
PhreeqcRM::MpiWorkerBreak()
{
#ifdef USE_MPI
	if (mpi_myself == 0)
	{
		int method = METHOD_MPIWORKERBREAK;
		MPI_Bcast(&method, 1, MPI_INT, 0, phreeqcrm_comm);
	}
#endif
	return IRM_OK;
}
/* ---------------------------------------------------------------------- */
IRM_RESULT
PhreeqcRM::OpenFiles(void)
/* ---------------------------------------------------------------------- */
{
	// opens error file, log file, and output file
	// error_file is stderr
	this->phreeqcrm_error_string.clear();
	IRM_RESULT return_value = IRM_OK;
	try
	{
		if (this->mpi_myself == 0)
		{
			// avoid memory leaks if already open
			this->CloseFiles();

			// open echo and log file, prefix.log.txt
			std::string ln = this->file_prefix;
			ln.append(".log.txt");
			if (!this->phreeqcrm_io->log_open(ln.c_str()))
			{
				this->ErrorHandler(IRM_FAIL, "Failed to open .log.txt file");
			}
			this->phreeqcrm_io->Set_log_on(true);

			// prefix.chem.txt
			std::string cn = this->file_prefix;
			cn.append(".chem.txt");
			if (!this->phreeqcrm_io->output_open(cn.c_str()))
			{
				this->ErrorHandler(IRM_FAIL, "Failed to open .chem.txt file");
			}
		}
	}
	catch (...)
	{
		return_value = IRM_FAIL;
	}
	return this->ReturnHandler(return_value, "PhreeqcRM::OpenFiles");
}
/* ---------------------------------------------------------------------- */
void
PhreeqcRM::OutputMessage(const std::string &str)
/* ---------------------------------------------------------------------- */
{
	this->phreeqcrm_io->output_msg(str.c_str());
}
/* ---------------------------------------------------------------------- */
void
PhreeqcRM::PartitionUZ(int n, int iphrq, int ihst, double new_frac)
/* ---------------------------------------------------------------------- */
{
	this->phreeqcrm_error_string.clear();
	int n_user;
	double s1, s2, uz1, uz2;

	/*
	 * repartition solids for partially saturated cells
	 */
#ifdef USE_MPI
	int l = iphrq - this->start_cell[mpi_myself];
	double old_frac = this->old_saturation_worker[l];
#else
	double old_frac = this->old_saturation_root[ihst];
#endif
	if (fabs(old_frac - new_frac) < 1e-8)
	{
		return;
	}

	n_user = iphrq;

	if (new_frac >= 1.0)
	{
		/* put everything in saturated zone */
		uz1 = 0;
		uz2 = 0;
		s1 = 1.0;
		s2 = 1.0;
	}
	else if (new_frac <= 1e-6)
	{
		/* put everything in unsaturated zone */
		uz1 = 1.0;
		uz2 = 1.0;
		s1 = 0.0;
		s2 = 0.0;
	}
	else if (new_frac > old_frac)
	{
		/* wetting cell */
		uz1 = 0.;
		uz2 = (1.0 - new_frac) / (1.0 - old_frac);
		s1 = 1.;
		s2 = 1.0 - uz2;
	}
	else
	{
		/* draining cell */
		s1 = new_frac / old_frac;
		s2 = 0.0;
		uz1 = 1.0 - s1;
		uz2 = 1.0;
	}
	cxxMix szmix, uzmix;
	szmix.Add(0, s1);
	szmix.Add(1, s2);
	uzmix.Add(0, uz1);
	uzmix.Add(1, uz2);
	/*
	 *   Calculate new compositions
	 */

	cxxStorageBin sz_bin;
	IPhreeqcPhast *phast_iphreeqc_worker = this->workers[n];
	phast_iphreeqc_worker->Put_cell_in_storage_bin(sz_bin, n_user);

//Exchange
	if (sz_bin.Get_Exchange(n_user) != NULL)
	{
		cxxStorageBin tempBin;
		tempBin.Set_Exchange(0, sz_bin.Get_Exchange(n_user));
		tempBin.Set_Exchange(1, phast_iphreeqc_worker->uz_bin.Get_Exchange(n_user));
		cxxExchange newsz(tempBin.Get_Exchangers(), szmix, n_user);
		cxxExchange newuz(tempBin.Get_Exchangers(), uzmix, n_user);
		sz_bin.Set_Exchange(n_user, &newsz);
		phast_iphreeqc_worker->uz_bin.Set_Exchange(n_user, &newuz);
	}
//PPassemblage
	if (sz_bin.Get_PPassemblage(n_user) != NULL)
	{
		cxxStorageBin tempBin;
		tempBin.Set_PPassemblage(0, sz_bin.Get_PPassemblage(n_user));
		tempBin.Set_PPassemblage(1, phast_iphreeqc_worker->uz_bin.Get_PPassemblage(n_user));
		cxxPPassemblage newsz(tempBin.Get_PPassemblages(), szmix, n_user);
		cxxPPassemblage newuz(tempBin.Get_PPassemblages(), uzmix, n_user);
		sz_bin.Set_PPassemblage(n_user, &newsz);
		phast_iphreeqc_worker->uz_bin.Set_PPassemblage(n_user, &newuz);
	}
//Gas_phase
	if (sz_bin.Get_GasPhase(n_user) != NULL)
	{
		cxxStorageBin tempBin;
		tempBin.Set_GasPhase(0, sz_bin.Get_GasPhase(n_user));
		tempBin.Set_GasPhase(1, phast_iphreeqc_worker->uz_bin.Get_GasPhase(n_user));
		cxxGasPhase newsz(tempBin.Get_GasPhases(), szmix, n_user);
		cxxGasPhase newuz(tempBin.Get_GasPhases(), uzmix, n_user);
		sz_bin.Set_GasPhase(n_user, &newsz);
		phast_iphreeqc_worker->uz_bin.Set_GasPhase(n_user, &newuz);
	}
//SSassemblage
	if (sz_bin.Get_SSassemblage(n_user) != NULL)
	{
		cxxStorageBin tempBin;
		tempBin.Set_SSassemblage(0, sz_bin.Get_SSassemblage(n_user));
		tempBin.Set_SSassemblage(1, phast_iphreeqc_worker->uz_bin.Get_SSassemblage(n_user));
		cxxSSassemblage newsz(tempBin.Get_SSassemblages(), szmix, n_user);
		cxxSSassemblage newuz(tempBin.Get_SSassemblages(), uzmix, n_user);
		sz_bin.Set_SSassemblage(n_user, &newsz);
		phast_iphreeqc_worker->uz_bin.Set_SSassemblage(n_user, &newuz);
	}
//Kinetics
	if (sz_bin.Get_Kinetics(n_user) != NULL)
	{
		cxxStorageBin tempBin;
		tempBin.Set_Kinetics(0, sz_bin.Get_Kinetics(n_user));
		tempBin.Set_Kinetics(1, phast_iphreeqc_worker->uz_bin.Get_Kinetics(n_user));
		cxxKinetics newsz(tempBin.Get_Kinetics(), szmix, n_user);
		cxxKinetics newuz(tempBin.Get_Kinetics(), uzmix, n_user);
		sz_bin.Set_Kinetics(n_user, &newsz);
		phast_iphreeqc_worker->uz_bin.Set_Kinetics(n_user, &newuz);
	}
//Surface
	if (sz_bin.Get_Surface(n_user) != NULL)
	{
		cxxStorageBin tempBin;
		tempBin.Set_Surface(0, sz_bin.Get_Surface(n_user));
		tempBin.Set_Surface(1, phast_iphreeqc_worker->uz_bin.Get_Surface(n_user));
		cxxSurface newsz(tempBin.Get_Surfaces(), szmix, n_user);
		cxxSurface newuz(tempBin.Get_Surfaces(), uzmix, n_user);
		sz_bin.Set_Surface(n_user, &newsz);
		phast_iphreeqc_worker->uz_bin.Set_Surface(n_user, &newuz);
	}

	// Put back in reaction module
	phast_iphreeqc_worker->Get_cell_from_storage_bin(sz_bin, n_user);

	/*
	 *   Eliminate uz if new fraction 1.0
	 */
	if (new_frac >= 1.0)
	{
		phast_iphreeqc_worker->uz_bin.Remove(iphrq);
	}
#ifdef USE_MPI
	this->old_saturation_worker[l] = new_frac;
#else
	this->old_saturation_root[ihst] = new_frac;
#endif
}
#ifdef USE_MPI
#define SERIALIZE
#ifdef SERIALIZE
/* ---------------------------------------------------------------------- */
void
PhreeqcRM::RebalanceLoad(void)
/* ---------------------------------------------------------------------- */
{
	if (this->mpi_tasks <= 1) return;
	if (this->mpi_tasks > count_chemistry) return;
	if (this->rebalance_fraction <= 0.0) return;
	if (this->rebalance_by_cell)
	{
		try
		{
			RebalanceLoadPerCell();
		}
		catch (...)
		{
			this->ErrorHandler(IRM_FAIL, "PhreeqcRM::RebalanceLoad");
		}
		return;
	}
#include <time.h>

	// working space
	std::vector<int> start_cell_new;
	std::vector<int> end_cell_new;
	for (int i = 0; i < this->mpi_tasks; i++)
	{
		start_cell_new.push_back(0);
		end_cell_new.push_back(0);
	}
	std::vector<int> cells_v;
	std::ostringstream error_stream;
	bool good_enough = false;

	// Calculate time per cell for this process
	IPhreeqcPhast * phast_iphreeqc_worker = this->workers[0];
	int cells = this->end_cell[this->mpi_myself] - this->start_cell[this->mpi_myself] + 1;
	double time_per_cell = phast_iphreeqc_worker->Get_thread_clock_time()/((double) cells);

	// Gather times at root
	std::vector<double> recv_buffer;
	recv_buffer.resize(this->mpi_tasks);
	MPI_Gather(&time_per_cell, 1, MPI_DOUBLE, &recv_buffer.front(), 1, MPI_DOUBLE, 0,
			   phreeqcrm_comm);

	IRM_RESULT return_value = IRM_OK;
	try
	{
		if (this->mpi_myself == 0)
		{
			double total = 0;
			for (int i = 0; i < this->mpi_tasks; i++)
			{
				total += recv_buffer[i];
			}
			double avg = total / (double) this->mpi_tasks;
			// Normalize
			total = 0;
			for (int i = 0; i < this->mpi_tasks; i++)
			{
				assert(recv_buffer[i] >= 0);
				if (recv_buffer[i] == 0) recv_buffer[i] = 0.25*avg;
				total += recv_buffer[0] / recv_buffer[i];
			}

			// Set first and last cells
			double new_n = this->count_chemistry / total; /* new_n is number of cells for root */


			// Calculate number of cells per process, rounded to lower number
			int	total_cells = 0;
			int n = 0;
			for (int i = 0; i < this->mpi_tasks; i++)
			{
				n = (int) floor(new_n * recv_buffer[0] / recv_buffer[i]);
				if (n < 1)
					n = 1;
				cells_v.push_back(n);
				total_cells += n;
			}

			// Distribute cells from rounding down
			int diff_cells = this->count_chemistry - total_cells;
			if (diff_cells > 0)
			{
				for (int j = 0; j < diff_cells; j++)
				{
					int min_cell = 0;
					double min_time = (cells_v[0] + 1) * recv_buffer[0];
					for (int i = 1; i < this->mpi_tasks; i++)
					{
						if ((cells_v[i] + 1) * recv_buffer[i] < min_time)
						{
							min_cell = i;
							min_time = (cells_v[i] + 1) * recv_buffer[i];
						}
					}
					cells_v[min_cell] += 1;
				}
			}
			else if (diff_cells < 0)
			{
				for (int j = 0; j < -diff_cells; j++)
				{
					int max_cell = -1;
					double max_time = 0;
					for (int i = 0; i < this->mpi_tasks; i++)
					{
						if (cells_v[i] > 1)
						{
							if ((cells_v[i] - 1) * recv_buffer[i] > max_time)
							{
								max_cell = i;
								max_time = (cells_v[i] - 1) * recv_buffer[i];
							}
						}
					}
					cells_v[max_cell] -= 1;
				}
			}

			// Fill in subcolumn ends
			int last = -1;
			for (int i = 0; i < this->mpi_tasks; i++)
			{
				start_cell_new[i] = last + 1;
				end_cell_new[i] = start_cell_new[i] + cells_v[i] - 1;
				last = end_cell_new[i];
			}

			// Check that all cells are distributed
			if (end_cell_new[this->mpi_tasks - 1] != this->count_chemistry - 1)
			{
				error_stream << "Failed: " << diff_cells << ", count_cells " << this->count_chemistry << ", last cell "
					<< end_cell_new[this->mpi_tasks - 1] << "\n";
				for (int i = 0; i < this->mpi_tasks; i++)
				{
					error_stream << i << ": first " << start_cell_new[i] << "\tlast " << end_cell_new[i] << "\n";
				}
				error_stream << "Failed to redistribute cells." << "\n";
				this->ErrorHandler(IRM_FAIL, error_stream.str().c_str());
			}

			// Compare old and new times
			double max_old = 0.0;
			double max_new = 0.0;
			for (int i = 0; i < this->mpi_tasks; i++)
			{
				double t = cells_v[i] * recv_buffer[i];
				if (t > max_new)
					max_new = t;
				t = (end_cell[i] - start_cell[i] + 1) * recv_buffer[i];
				if (t > max_old)
					max_old = t;
			}
			{
				//std::cerr << "          Estimated efficiency of chemistry " << (float) ((LDBLE) 100. * max_new / max_old) << "\n";
				std::ostringstream msg;
				msg << "          Estimated efficiency of chemistry " << (float) ((LDBLE) 100. * max_new / max_old) << "\n";
				this->ScreenMessage(msg.str().c_str());
			}

			if ((max_old - max_new) / max_old < 0.05)
			{
				for (int i = 0; i < this->mpi_tasks; i++)
				{
					start_cell_new[i] = start_cell[i];
					end_cell_new[i] = end_cell[i];
				}
				good_enough = true;
			}
			else
			{
				for (int i = 0; i < this->mpi_tasks - 1; i++)
				{
					int icells = (int) ((end_cell_new[i] - end_cell[i]) * this->rebalance_fraction);
					end_cell_new[i] = end_cell[i] + icells;
					start_cell_new[i + 1] = end_cell_new[i] + 1;
				}
			}
		}
	}
	catch (...)
	{
		return_value = IRM_FAIL;
	}

	// Broadcast error condition
	MPI_Bcast(&return_value, 1, MPI_INT, 0, phreeqcrm_comm);

	/*
	 *   Broadcast new subcolumns
	 */

	if (return_value == 0)
	{
		try
		{
			std::vector<int> r_vector;
			r_vector.push_back(0);

			MPI_Bcast((void *) &start_cell_new.front(), mpi_tasks, MPI_INT, 0, phreeqcrm_comm);
			MPI_Bcast((void *) &end_cell_new.front(), mpi_tasks, MPI_INT, 0, phreeqcrm_comm);

			/*
			*   Redefine columns
			*/
			int nnew = 0;
			int old = 0;
			int change = 0;
			std::map< std::string, std::vector<int> > transfer_pair;
			for (int k = 0; k < this->count_chemistry; k++)
			{
				while (k > end_cell[old])
				{
					old++;
				}
				while (k > end_cell_new[nnew])
				{
					nnew++;
				}

				if (old == nnew)
					continue;
				change++;
				std::ostringstream key;
				key << old << "#" << nnew;
				std::map< std::string, std::vector<int> >::iterator tp_it = transfer_pair.find(key.str());
				if (tp_it == transfer_pair.end())
				{
					std::vector<int> v;
					v.push_back(old);
					v.push_back(nnew);
					transfer_pair[key.str()] = v;
				}
				transfer_pair[key.str()].push_back(k);
			}
			std::map< std::string, std::vector<int> >::iterator tp_it = transfer_pair.begin();

			// Transfer cells
			int transfers = 0;
			for ( ; tp_it != transfer_pair.end(); tp_it++)
			{
				Serializer serial;
				int pold = tp_it->second[0];
				int pnew = tp_it->second[1];
				// transfer cells from pold to pnew
				try
				{
					if (this->mpi_myself == pold)
					{
						for (size_t i = 2; i < tp_it->second.size(); i++)
						{
							int k = tp_it->second[i];
							serial.Serialize(*phast_iphreeqc_worker->Get_PhreeqcPtr(), k, k, false, false);
						}
						int total_size = (int) (serial.GetDictionary().GetDictionaryOss().str().size() * sizeof(char));
						total_size += (int) (serial.GetInts().size() * sizeof(int));
						total_size += (int) (serial.GetDoubles().size() * sizeof(double));
						total_size += 1000;
						int position=0;
						char *buffer = new char[total_size];
						MPI_Pack((void *)serial.GetDictionary().GetDictionaryOss().str().c_str(), (int) serial.GetDictionary().GetDictionaryOss().str().size(), 
							MPI_CHAR, buffer, total_size, &position, MPI_COMM_WORLD);
						MPI_Pack((void *)&(serial.GetInts()[0]), (int) serial.GetInts().size(), 
							MPI_INT, buffer, total_size, &position, MPI_COMM_WORLD);
						MPI_Pack((void *)&(serial.GetDoubles()[0]), (int) serial.GetDoubles().size(), 
							MPI_DOUBLE, buffer, total_size, &position, MPI_COMM_WORLD);
						int sizes[4];
						sizes[0] = position;
						sizes[1] = (int) serial.GetDictionary().GetDictionaryOss().str().size();
						sizes[2] = (int) serial.GetInts().size();
						sizes[3] = (int) serial.GetDoubles().size();
						MPI_Send(&sizes, 4, MPI_INT, pnew, 0, MPI_COMM_WORLD);
						MPI_Send(buffer, position, MPI_PACKED, pnew, 0, MPI_COMM_WORLD);
						delete [] buffer;
					}
					else if (this->GetMpiMyself() == pnew)
					{
						MPI_Status mpi_status;
						int sizes[4];
						MPI_Recv(&sizes[0], 4, MPI_INT, pold, 0, MPI_COMM_WORLD, &mpi_status);

						Serializer serial;
						std::string  string_buffer;
						string_buffer.resize(sizes[1]);
						std::vector<int> ints;
						ints.resize(sizes[2], 0);
						std::vector<double> doubles;
						doubles.resize(sizes[3],0.0);
						char *buffer = new char[sizes[0]];
						MPI_Recv((void *) buffer, sizes[0], MPI_PACKED, pold, 0, MPI_COMM_WORLD, &mpi_status);

						int position = 0;
						MPI_Unpack(buffer, sizes[0], &position, &(string_buffer[0]), sizes[1],
							MPI_CHAR, MPI_COMM_WORLD);
						MPI_Unpack(buffer, sizes[0], &position, &(ints[0]), sizes[2],
							MPI_INT, MPI_COMM_WORLD);
						MPI_Unpack(buffer, sizes[0], &position, &(doubles[0]), sizes[3],
							MPI_DOUBLE, MPI_COMM_WORLD);

						Dictionary dictionary(string_buffer);
						IPhreeqcPhast * phast_iphreeqc_worker = this->workers[0];
						serial.Deserialize(*phast_iphreeqc_worker->Get_PhreeqcPtr(), dictionary, ints, doubles);
						delete [] buffer;
					}
					transfers++;
				}
				catch (...)
				{
					r_vector[0] = 1;
				}		
				// delete cells from pold
				if (this->mpi_myself == pold && r_vector[0] == 0)
				{
					std::ostringstream del;
					del << "DELETE; -cell\n";
					for (size_t i = 2; i < tp_it->second.size(); i++)
					{
						del << tp_it->second[i] << "\n";

					}
					try
					{
						int status = phast_iphreeqc_worker->RunString(del.str().c_str());
						if (status != 0)
						{
							this->ErrorMessage(phast_iphreeqc_worker->GetErrorString());
						}
						this->ErrorHandler(PhreeqcRM::Int2IrmResult(status, false), "RunString");
					}
					catch (...)
					{
						r_vector[0] = 1;
					}
				}

				// Also need to tranfer UZ
				if (this->partition_uz_solids)
				{
					std::ostringstream uz_dump;
					if (this->mpi_myself == pold)
					{
						for (size_t i = 2; i < tp_it->second.size(); i++)
						{
							int k = tp_it->second[i];
							phast_iphreeqc_worker->uz_bin.Remove_Solution(k);
							phast_iphreeqc_worker->uz_bin.dump_raw(uz_dump, k, 0);
							phast_iphreeqc_worker->uz_bin.Remove(k);
						}
					}
					try
					{
						this->TransferCellsUZ(uz_dump, pold, pnew);
					}
					catch (...)
					{
						r_vector[0] = 1;
					}
				}

				//The gather is sometimes slow for some reason
				//this->HandleErrorsInternal(r_vector);
				if (r_vector[0] != 0)
					throw PhreeqcRMStop();
			}
			for (int i = 0; i < this->mpi_tasks; i++)
			{
				start_cell[i] = start_cell_new[i];
				end_cell[i] = end_cell_new[i];
			}
			if (this->mpi_myself == 0)
			{
				//std::cerr << "          Cells shifted between processes     " << change << "\n";
				std::ostringstream msg;
				msg << "          Cells shifted between processes     " << change << "\n";
				this->ScreenMessage(msg.str().c_str());
			}
			if (change > 0)
			{
				ScatterNchem(print_chem_mask_root, print_chem_mask_worker);
				ScatterNchem(density_root, density_worker);
				//ScatterNchem(tempc_root, tempc_worker);
				ScatterNchem(porosity_root, porosity_worker);
				ScatterNchem(rv_root, rv_worker);
				//ScatterNchem(pressure_root, pressure_worker);
				ScatterNchem(saturation_root, saturation_worker);
				if (partition_uz_solids)
				{
					ScatterNchem(old_saturation_root, old_saturation_worker);
				}
			}
		}
		catch (...)
		{
			return_value = IRM_FAIL;
		}
	}
	this->ErrorHandler(return_value, "PhreeqcRM::RebalanceLoad");
}
#else
/* ---------------------------------------------------------------------- */
void
PhreeqcRM::RebalanceLoad(void)
/* ---------------------------------------------------------------------- */
{
	if (this->mpi_tasks <= 1) return;
	if (this->mpi_tasks > count_chemistry) return;
	if (this->rebalance_fraction <= 0.0) return;
	if (this->rebalance_by_cell)
	{
		try
		{
			RebalanceLoadPerCell();
		}
		catch (...)
		{
			this->ErrorHandler(IRM_FAIL, "PhreeqcRM::RebalanceLoad");
		}
		return;
	}
#include <time.h>

	// working space
	std::vector<int> start_cell_new;
	std::vector<int> end_cell_new;
	for (int i = 0; i < this->mpi_tasks; i++)
	{
		start_cell_new.push_back(0);
		end_cell_new.push_back(0);
	}
	std::vector<int> cells_v;
	std::ostringstream error_stream;
	bool good_enough = false;

	// Calculate time per cell for this process
	IPhreeqcPhast * phast_iphreeqc_worker = this->workers[0];
	int cells = this->end_cell[this->mpi_myself] - this->start_cell[this->mpi_myself] + 1;
	double time_per_cell = phast_iphreeqc_worker->Get_thread_clock_time()/((double) cells);

	// Gather times at root
	std::vector<double> recv_buffer;
	recv_buffer.resize(this->mpi_tasks);
	MPI_Gather(&time_per_cell, 1, MPI_DOUBLE, &recv_buffer.front(), 1, MPI_DOUBLE, 0,
			   phreeqcrm_comm);

	IRM_RESULT return_value = IRM_OK;
	try
	{
		if (this->mpi_myself == 0)
		{
			double total = 0;
			for (int i = 0; i < this->mpi_tasks; i++)
			{
				total += recv_buffer[i];
			}
			double avg = total / (double) this->mpi_tasks;
			// Normalize
			total = 0;
			for (int i = 0; i < this->mpi_tasks; i++)
			{
				assert(recv_buffer[i] >= 0);
				if (recv_buffer[i] == 0) recv_buffer[i] = 0.25*avg;
				total += recv_buffer[0] / recv_buffer[i];
			}

			// Set first and last cells
			double new_n = this->count_chemistry / total; /* new_n is number of cells for root */


			// Calculate number of cells per process, rounded to lower number
			int	total_cells = 0;
			int n = 0;
			for (int i = 0; i < this->mpi_tasks; i++)
			{
				n = (int) floor(new_n * recv_buffer[0] / recv_buffer[i]);
				if (n < 1)
					n = 1;
				cells_v.push_back(n);
				total_cells += n;
			}

			// Distribute cells from rounding down
			int diff_cells = this->count_chemistry - total_cells;
			if (diff_cells > 0)
			{
				for (int j = 0; j < diff_cells; j++)
				{
					int min_cell = 0;
					double min_time = (cells_v[0] + 1) * recv_buffer[0];
					for (int i = 1; i < this->mpi_tasks; i++)
					{
						if ((cells_v[i] + 1) * recv_buffer[i] < min_time)
						{
							min_cell = i;
							min_time = (cells_v[i] + 1) * recv_buffer[i];
						}
					}
					cells_v[min_cell] += 1;
				}
			}
			else if (diff_cells < 0)
			{
				for (int j = 0; j < -diff_cells; j++)
				{
					int max_cell = -1;
					double max_time = 0;
					for (int i = 0; i < this->mpi_tasks; i++)
					{
						if (cells_v[i] > 1)
						{
							if ((cells_v[i] - 1) * recv_buffer[i] > max_time)
							{
								max_cell = i;
								max_time = (cells_v[i] - 1) * recv_buffer[i];
							}
						}
					}
					cells_v[max_cell] -= 1;
				}
			}

			// Fill in subcolumn ends
			int last = -1;
			for (int i = 0; i < this->mpi_tasks; i++)
			{
				start_cell_new[i] = last + 1;
				end_cell_new[i] = start_cell_new[i] + cells_v[i] - 1;
				last = end_cell_new[i];
			}

			// Check that all cells are distributed
			if (end_cell_new[this->mpi_tasks - 1] != this->count_chemistry - 1)
			{
				error_stream << "Failed: " << diff_cells << ", count_cells " << this->count_chemistry << ", last cell "
					<< end_cell_new[this->mpi_tasks - 1] << "\n";
				for (int i = 0; i < this->mpi_tasks; i++)
				{
					error_stream << i << ": first " << start_cell_new[i] << "\tlast " << end_cell_new[i] << "\n";
				}
				error_stream << "Failed to redistribute cells." << "\n";
				this->ErrorHandler(IRM_FAIL, error_stream.str().c_str());
			}

			// Compare old and new times
			double max_old = 0.0;
			double max_new = 0.0;
			for (int i = 0; i < this->mpi_tasks; i++)
			{
				double t = cells_v[i] * recv_buffer[i];
				if (t > max_new)
					max_new = t;
				t = (end_cell[i] - start_cell[i] + 1) * recv_buffer[i];
				if (t > max_old)
					max_old = t;
			}
			{
				//std::cerr << "          Estimated efficiency of chemistry " << (float) ((LDBLE) 100. * max_new / max_old) << "\n";
				std::ostringstream msg;
				msg << "          Estimated efficiency of chemistry " << (float) ((LDBLE) 100. * max_new / max_old) << "\n";
				this->ScreenMessage(msg.str().c_str());
			}

			if ((max_old - max_new) / max_old < 0.05)
			{
				for (int i = 0; i < this->mpi_tasks; i++)
				{
					start_cell_new[i] = start_cell[i];
					end_cell_new[i] = end_cell[i];
				}
				good_enough = true;
			}
			else
			{
				for (int i = 0; i < this->mpi_tasks - 1; i++)
				{
					int icells = (int) ((end_cell_new[i] - end_cell[i]) * this->rebalance_fraction);
					end_cell_new[i] = end_cell[i] + icells;
					start_cell_new[i + 1] = end_cell_new[i] + 1;
				}
			}
		}
	}
	catch (...)
	{
		return_value = IRM_FAIL;
	}

	// Broadcast error condition
	MPI_Bcast(&return_value, 1, MPI_INT, 0, phreeqcrm_comm);

	/*
	 *   Broadcast new subcolumns
	 */

	if (return_value == 0)
	{
		try
		{
			std::vector<int> r_vector;
			r_vector.push_back(0);

			MPI_Bcast((void *) &start_cell_new.front(), mpi_tasks, MPI_INT, 0, phreeqcrm_comm);
			MPI_Bcast((void *) &end_cell_new.front(), mpi_tasks, MPI_INT, 0, phreeqcrm_comm);

			/*
			*   Redefine columns
			*/
			int nnew = 0;
			int old = 0;
			int change = 0;
			std::map< std::string, std::vector<int> > transfer_pair;
			for (int k = 0; k < this->count_chemistry; k++)
			{
				while (k > end_cell[old])
				{
					old++;
				}
				while (k > end_cell_new[nnew])
				{
					nnew++;
				}

				if (old == nnew)
					continue;
				change++;
				std::ostringstream key;
				key << old << "#" << nnew;
				std::map< std::string, std::vector<int> >::iterator tp_it = transfer_pair.find(key.str());
				if (tp_it == transfer_pair.end())
				{
					std::vector<int> v;
					v.push_back(old);
					v.push_back(nnew);
					transfer_pair[key.str()] = v;
				}
				transfer_pair[key.str()].push_back(k);
			}
			std::map< std::string, std::vector<int> >::iterator tp_it = transfer_pair.begin();

			// Transfer cells
			int transfers = 0;
			for ( ; tp_it != transfer_pair.end(); tp_it++)
			{
				cxxStorageBin t_bin;
				int pold = tp_it->second[0];
				int pnew = tp_it->second[1];
				if (this->mpi_myself == pold)
				{
					for (size_t i = 2; i < tp_it->second.size(); i++)
					{
						int k = tp_it->second[i];
						phast_iphreeqc_worker->Get_PhreeqcPtr()->phreeqc2cxxStorageBin(t_bin, k);
					}
				}
				transfers++;
				try
				{
					this->TransferCells(t_bin, pold, pnew);
				}
				catch (...)
				{
					r_vector[0] = 1;
				}

				// Put cell in t_bin
				if (this->mpi_myself == pold && r_vector[0] == 0)
				{
					std::ostringstream del;
					del << "DELETE; -cell\n";
					for (size_t i = 2; i < tp_it->second.size(); i++)
					{
						del << tp_it->second[i] << "\n";

					}
					try
					{
						int status = phast_iphreeqc_worker->RunString(del.str().c_str());
						if (status != 0)
						{
							this->ErrorMessage(phast_iphreeqc_worker->GetErrorString());
						}
						this->ErrorHandler(PhreeqcRM::Int2IrmResult(status, false), "RunString");
					}
					catch (...)
					{
						r_vector[0] = 1;
					}
				}

				// Also need to tranfer UZ
				if (this->partition_uz_solids)
				{
					std::ostringstream uz_dump;
					if (this->mpi_myself == pold)
					{
						for (size_t i = 2; i < tp_it->second.size(); i++)
						{
							int k = tp_it->second[i];
							phast_iphreeqc_worker->uz_bin.Remove_Solution(k);
							phast_iphreeqc_worker->uz_bin.dump_raw(uz_dump, k, 0);
							phast_iphreeqc_worker->uz_bin.Remove(k);
						}
					}
					try
					{
						this->TransferCellsUZ(uz_dump, pold, pnew);
					}
					catch (...)
					{
						r_vector[0] = 1;
					}
				}

				//The gather is sometimes slow for some reason
				//this->HandleErrorsInternal(r_vector);
				if (r_vector[0] != 0)
					throw PhreeqcRMStop();
			}
			for (int i = 0; i < this->mpi_tasks; i++)
			{
				start_cell[i] = start_cell_new[i];
				end_cell[i] = end_cell_new[i];
			}
			if (this->mpi_myself == 0)
			{
				//std::cerr << "          Cells shifted between processes     " << change << "\n";
				std::ostringstream msg;
				msg << "          Cells shifted between processes     " << change << "\n";
				this->ScreenMessage(msg.str().c_str());
			}
			if (change > 0)
			{
				ScatterNchem(print_chem_mask_root, print_chem_mask_worker);
				ScatterNchem(density_root, density_worker);
				//ScatterNchem(tempc_root, tempc_worker);
				ScatterNchem(porosity_root, porosity_worker);
				ScatterNchem(rv_root, rv_worker);
				//ScatterNchem(pressure_root, pressure_worker);
				ScatterNchem(saturation_root, saturation_worker);
				if (partition_uz_solids)
				{
					ScatterNchem(old_saturation_root, old_saturation_worker);
				}
			}
		}
		catch (...)
		{
			return_value = IRM_FAIL;
		}
	}
	this->ErrorHandler(return_value, "PhreeqcRM::RebalanceLoad");
}
#endif
#else
/* ---------------------------------------------------------------------- */
void
PhreeqcRM::RebalanceLoad(void)
/* ---------------------------------------------------------------------- */
{
	// Threaded version
	if (this->nthreads <= 1) return;
	if (this->rebalance_fraction <= 0.0) return;
	if (this->nthreads > count_chemistry) return;
#include <time.h>
	if (this->rebalance_by_cell)
	{
		try
		{
			RebalanceLoadPerCell();
		}
		catch (...)
		{
			this->ErrorHandler(IRM_FAIL, "PhreeqcRM::RebalanceLoad");
		}
		return;
	}
	std::vector<int> start_cell_new;
	std::vector<int> end_cell_new;
	for (int i = 0; i < this->nthreads; i++)
	{
		start_cell_new.push_back(0);
		end_cell_new.push_back(0);
	}

	std::vector<int> cells_v;
	std::ostringstream error_stream;
	/*
	 *  Gather times of all tasks
	 */
	std::vector<double> recv_buffer;
	double total = 0;
	for (int i = 0; i < this->nthreads; i++)
	{
		IPhreeqcPhast * phast_iphreeqc_worker = this->workers[i];
		recv_buffer.push_back(phast_iphreeqc_worker->Get_thread_clock_time());
		total += recv_buffer[i];
	}
	double avg = total / (double) this->nthreads;

	// Normalize
	for (int i = 0; i < this->nthreads; i++)
	{
		assert(recv_buffer[i] >= 0);
		if (recv_buffer[i] == 0) recv_buffer[i] = 0.25*avg;
		int cells = this->end_cell[i] - this->start_cell[i] + 1;
		recv_buffer[i] /= (double) cells;
	}

	// Calculate total
	total = 0;
	for (int i = 0; i < this->nthreads; i++)
	{
		total += recv_buffer[0] / recv_buffer[i];
	}

	/*
	 *  Set first and last cells
	 */
	double new_n = this->count_chemistry / total; /* new_n is number of cells for root */
	int	total_cells = 0;
	int n = 0;
	/*
	*  Calculate number of cells per process, rounded to lower number
	*/
	for (int i = 0; i < this->nthreads; i++)
	{
		n = (int) floor(new_n * recv_buffer[0] / recv_buffer[i]);
		if (n < 1)
			n = 1;
		cells_v.push_back(n);
		total_cells += n;
	}
	/*
	*  Distribute cells from rounding down
	*/
	int diff_cells = this->count_chemistry - total_cells;
	if (diff_cells > 0)
	{
		for (int j = 0; j < diff_cells; j++)
		{
			int min_cell = 0;
			double min_time = (cells_v[0] + 1) * recv_buffer[0];
			for (int i = 1; i < this->nthreads; i++)
			{
				if ((cells_v[i] + 1) * recv_buffer[i] < min_time)
				{
					min_cell = i;
					min_time = (cells_v[i] + 1) * recv_buffer[i];
				}
			}
			cells_v[min_cell] += 1;
		}
	}
	else if (diff_cells < 0)
	{
		for (int j = 0; j < -diff_cells; j++)
		{
			int max_cell = -1;
			double max_time = 0;
			for (int i = 0; i < this->nthreads; i++)
			{
				if (cells_v[i] > 1)
				{
					if ((cells_v[i] - 1) * recv_buffer[i] > max_time)
					{
						max_cell = i;
						max_time = (cells_v[i] - 1) * recv_buffer[i];
					}
				}
			}
			cells_v[max_cell] -= 1;
		}
	}
	/*
	*  Fill in subcolumn ends
	*/
	int last = -1;
	for (int i = 0; i < this->nthreads; i++)
	{
		start_cell_new[i] = last + 1;
		end_cell_new[i] = start_cell_new[i] + cells_v[i] - 1;
		last = end_cell_new[i];
	}
	/*
	*  Check that all cells are distributed
	*/
	try
	{
		if (end_cell_new[this->nthreads - 1] != this->count_chemistry - 1)
		{
			error_stream << "Failed: " << diff_cells << ", count_cells " << this->count_chemistry << ", last cell "
				<< end_cell_new[this->nthreads - 1] << "\n";
			for (int i = 0; i < this->nthreads; i++)
			{
				error_stream << i << ": first " << start_cell_new[i] << "\tlast " << end_cell_new[i] << "\n";
			}
			error_stream << "Failed to redistribute cells." << "\n";
			this->ErrorHandler(IRM_FAIL, error_stream.str().c_str());
		}
		/*
		*   Compare old and new times
		*/
		double max_old = 0.0;
		double max_new = 0.0;
		for (int i = 0; i < this->nthreads; i++)
		{
			double t = cells_v[i] * recv_buffer[i];
			if (t > max_new)
				max_new = t;
			t = (end_cell[i] - start_cell[i] + 1) * recv_buffer[i];
			if (t > max_old)
				max_old = t;
		}
		{
			//std::cerr << "          Estimated efficiency of chemistry " << (float) ((LDBLE) 100. * max_new / max_old) << "\n";
			std::ostringstream msg;
			msg << "          Estimated efficiency of chemistry " << (float) ((LDBLE) 100. * max_new / max_old) << "\n";
			this->ScreenMessage(msg.str().c_str());
		}
		if ((max_old - max_new) / max_old < 0.05)
		{
			for (int i = 0; i < this->nthreads; i++)
			{
				start_cell_new[i] = start_cell[i];
				end_cell_new[i] = end_cell[i];
			}
		}
		else
		{
			for (int i = 0; i < this->nthreads - 1; i++)
			{
				int icells = (int) ((end_cell_new[i] - end_cell[i]) * this->rebalance_fraction);
				end_cell_new[i] = end_cell[i] + icells;
				start_cell_new[i + 1] = end_cell_new[i] + 1;
			}
		}
		/*
		*   Redefine columns
		*/
		int nnew = 0;
		int old = 0;
		int change = 0;

		for (int k = 0; k < this->count_chemistry; k++)
		{
			int i = k;
			int iphrq = i;			/* iphrq is 1 to count_chem */
			while (k > end_cell[old])
			{
				old++;
			}
			while (k > end_cell_new[nnew])
			{
				nnew++;
			}

			if (old == nnew)
				continue;
			change++;
			IPhreeqcPhast * old_worker = this->workers[old];
			IPhreeqcPhast * new_worker = this->workers[nnew];
			cxxStorageBin temp_bin;
			old_worker->Get_PhreeqcPtr()->phreeqc2cxxStorageBin(temp_bin, iphrq);
			new_worker->Get_PhreeqcPtr()->cxxStorageBin2phreeqc(temp_bin, iphrq);
			std::ostringstream del;
			del << "DELETE; -cell " << iphrq << "\n";
			int status = old_worker->RunString(del.str().c_str());
			if (status != 0)
			{
				this->ErrorMessage(old_worker->GetErrorString());
			}
			this->ErrorHandler(PhreeqcRM::Int2IrmResult(status, false), "RunString");

			// Also need to tranfer UZ
			if (this->partition_uz_solids)
			{
				new_worker->uz_bin.Add(old_worker->uz_bin, iphrq);
				old_worker->uz_bin.Remove(iphrq);
			}

		}

		for (int i = 0; i < this->nthreads; i++)
		{
			start_cell[i] = start_cell_new[i];
			end_cell[i] = end_cell_new[i];
			IPhreeqcPhast * worker = this->workers[i];
			worker->Set_start_cell(start_cell_new[i]);
			worker->Set_end_cell(end_cell_new[i]);
		}
		{
			//std::cerr << "          Cells shifted between threads     " << change << "\n";
			std::ostringstream msg;
			msg << "          Cells shifted between threads     " << change << "\n";
			this->ScreenMessage(msg.str().c_str());
		}
	}
	catch (...)
	{
		this->ErrorHandler(IRM_FAIL, "PhreeqcRM::RebalanceLoad");
	}
}
#endif
#ifdef USE_MPI
#ifdef SERIALIZE
/* ---------------------------------------------------------------------- */
void
PhreeqcRM::RebalanceLoadPerCell(void)
/* ---------------------------------------------------------------------- */
{
	// Throws on error
	if (this->mpi_tasks <= 1) return;
	if (this->mpi_tasks > count_chemistry) return;
#include <time.h>

	// vectors for each cell (count_chem)
	std::vector<double> recv_cell_times, normalized_cell_times;
	recv_cell_times.resize(this->count_chemistry);

	// vectors for each process (mpi_tasks)
	std::vector<double> standard_time, task_fraction, task_time;

	// Assume homogeneous cluster for now
	if (mpi_myself == 0)
	{
		double tasks_total = 0;
		for (size_t i = 0; i < (size_t) mpi_tasks; i++)
		{
			standard_time.push_back(this->standard_task_vector[i]);   // slower is bigger number
			//standard_time.push_back(1.0);                           // homogeneous
			tasks_total += 1.0 / standard_time[i];
		}

		for (size_t i = 0; i < (size_t) mpi_tasks; i++)
		{
			task_fraction.push_back((1.0 / standard_time[i]) / tasks_total);
		}
	}
	// Collect times
	IPhreeqcPhast * phast_iphreeqc_worker = this->workers[0];
	// manager
	if (mpi_myself == 0)
	{
		recv_cell_times.insert(recv_cell_times.begin(),
			phast_iphreeqc_worker->Get_cell_clock_times().begin(),
			phast_iphreeqc_worker->Get_cell_clock_times().end());
	}

	// workers
	for (int i = 1; i < mpi_tasks; i++)
	{
		int n = end_cell[i] - start_cell[i] + 1;
		if (mpi_myself == i)
		{
			MPI_Send(&phast_iphreeqc_worker->Get_cell_clock_times().front(), n, MPI_DOUBLE, 0, 0, phreeqcrm_comm);
		}
		if (mpi_myself == 0)
		{
			MPI_Status mpi_status;
			MPI_Recv((void *) &recv_cell_times[start_cell[i]], n, MPI_DOUBLE, i, 0, phreeqcrm_comm, &mpi_status);
		}
	}
	phast_iphreeqc_worker->Get_cell_clock_times().clear();
	// Root normalizes times, calculates efficiency, rebalances work
	double normalized_total_time = 0;
	double max_task_time = 0;
	// working space
	std::vector<int> start_cell_new;
	std::vector<int> end_cell_new;
	start_cell_new.resize(mpi_tasks, 0);
	end_cell_new.resize(mpi_tasks, 0);

	if (mpi_myself == 0)
	{
		// Normalize times
		max_task_time = 0;
		for (size_t i = 0; i < (size_t) mpi_tasks; i++)
		{
			double task_sum = 0;
			// normalize cell_times with standard_time
			for (size_t j = (size_t) start_cell[i]; j <= (size_t) end_cell[i]; j++)
			{
				task_sum += recv_cell_times[j];
				normalized_cell_times.push_back(recv_cell_times[j]/standard_time[i]);
				normalized_total_time += normalized_cell_times.back();
			}
			task_time.push_back(task_sum);
			max_task_time = (task_sum > max_task_time) ? task_sum : max_task_time;
		}

		// calculate efficiency
		double efficiency = 0;
		for (size_t i = 0; i < (size_t) mpi_tasks; i++)
		{
			efficiency += task_time[i] / max_task_time * task_fraction[i];
		}
		{
			//std::cerr << "          Estimated efficiency of chemistry without communication: " <<
			//		   (float) (100. * efficiency) << "\n";
			std::ostringstream msg;
			msg << "          Estimated efficiency of chemistry without communication: " <<
					   (float) (100. * efficiency) << "\n";
			this->ScreenMessage(msg.str().c_str());
		}

		// Split up work
		double f_low, f_high;
		f_high = 1 + 0.5 / ((double) mpi_tasks);
		f_low = 1;
		int j = 0;
		for (size_t i = 0; i < (size_t) mpi_tasks - 1; i++)
		{
			if (i > 0)
			{
				start_cell_new[i] = end_cell_new[i - 1] + 1;
			}
			double sum_work = 0;
			double temp_sum_work = 0;
			bool next = true;
			while (next)
			{
				temp_sum_work += normalized_cell_times[j] / normalized_total_time;
				if ((temp_sum_work < task_fraction[i]) && (((size_t) count_chemistry - j) > (size_t) (mpi_tasks - i)))
				{
					sum_work = temp_sum_work;
					j++;
					next = true;
				}
				else
				{
					if (j == start_cell_new[i])
					{
						end_cell_new[i] = j;
						j++;
					}
					else
					{
						end_cell_new[i] = j - 1;
					}
					next = false;
				}
			}
		}
		assert(j < count_chemistry);
		assert(mpi_tasks > 1);
		start_cell_new[mpi_tasks - 1] = end_cell_new[mpi_tasks - 2] + 1;
		end_cell_new[mpi_tasks - 1] = count_chemistry - 1;

		if (efficiency > 0.95)
		{
			for (int i = 0; i < this->mpi_tasks; i++)
			{
				start_cell_new[i] = start_cell[i];
				end_cell_new[i] = end_cell[i];
			}
		}
		else
		{
			for (size_t i = 0; i < (size_t) this->mpi_tasks - 1; i++)
			{
				int	icells;
				icells = (int) (((double) (end_cell_new[i] - end_cell[i])) * (this->rebalance_fraction) );
				if (icells == 0)
				{
					icells = end_cell_new[i] - end_cell[i];
				}
				end_cell_new[i] = end_cell[i] + icells;
				start_cell_new[i + 1] = end_cell_new[i] + 1;
			}
		}

	}

	/*
	 *   Broadcast new subcolumns
	 */

	MPI_Bcast((void *) &start_cell_new.front(), mpi_tasks, MPI_INT, 0, phreeqcrm_comm);
	MPI_Bcast((void *) &end_cell_new.front(), mpi_tasks, MPI_INT, 0, phreeqcrm_comm);

	/*
	 *   Redefine columns
	 */
	int nnew = 0;
	int old = 0;
	int change = 0;

			std::map< std::string, std::vector<int> > transfer_pair;
	for (int k = 0; k < this->count_chemistry; k++)
	{
		while (k > end_cell[old])
		{
			old++;
		}
		while (k > end_cell_new[nnew])
		{
			nnew++;
		}

		if (old == nnew)
			continue;
		change++;

		// Need to send cell from old task to nnew task
		std::ostringstream key;
		key << old << "#" << nnew;
		std::map< std::string, std::vector<int> >::iterator tp_it = transfer_pair.find(key.str());
		if (tp_it == transfer_pair.end())
		{
			std::vector<int> v;
			v.push_back(old);
			v.push_back(nnew);
			transfer_pair[key.str()] = v;
		}
		transfer_pair[key.str()].push_back(k);
	}

	// Transfer cells
	int transfers = 0;
	try
	{
		std::map< std::string, std::vector<int> >::iterator tp_it = transfer_pair.begin();
		std::vector<int> r_vector;
		r_vector.push_back(IRM_OK);
		for ( ; tp_it != transfer_pair.end(); tp_it++)
		{
			Serializer serial;
			int pold = tp_it->second[0];
			int pnew = tp_it->second[1];
			// transfer cells from pold to pnew
			try
			{
				if (this->mpi_myself == pold)
				{
					for (size_t i = 2; i < tp_it->second.size(); i++)
					{
						int k = tp_it->second[i];
						serial.Serialize(*phast_iphreeqc_worker->Get_PhreeqcPtr(), k, k, false, false);
					}
					int total_size = (int) (serial.GetDictionary().GetDictionaryOss().str().size() * sizeof(char));
					total_size += (int) (serial.GetInts().size() * sizeof(int));
					total_size += (int) (serial.GetDoubles().size() * sizeof(double));
					total_size += 1000;
					int position=0;
					char *buffer = new char[total_size];
					MPI_Pack((void *)serial.GetDictionary().GetDictionaryOss().str().c_str(), (int) serial.GetDictionary().GetDictionaryOss().str().size(), 
						MPI_CHAR, buffer, total_size, &position, MPI_COMM_WORLD);
					MPI_Pack((void *)&(serial.GetInts()[0]), (int) serial.GetInts().size(), 
						MPI_INT, buffer, total_size, &position, MPI_COMM_WORLD);
					MPI_Pack((void *)&(serial.GetDoubles()[0]), (int) serial.GetDoubles().size(), 
						MPI_DOUBLE, buffer, total_size, &position, MPI_COMM_WORLD);
					int sizes[4];
					sizes[0] = position;
					sizes[1] = (int) serial.GetDictionary().GetDictionaryOss().str().size();
					sizes[2] = (int) serial.GetInts().size();
					sizes[3] = (int) serial.GetDoubles().size();
					MPI_Send(&sizes, 4, MPI_INT, pnew, 0, MPI_COMM_WORLD);
					MPI_Send(buffer, position, MPI_PACKED, pnew, 0, MPI_COMM_WORLD);
					delete [] buffer;
				}
				else if (this->GetMpiMyself() == pnew)
				{
					MPI_Status mpi_status;
					int sizes[4];
					MPI_Recv(&sizes[0], 4, MPI_INT, pold, 0, MPI_COMM_WORLD, &mpi_status);

					Serializer serial;
					std::string  string_buffer;
					string_buffer.resize(sizes[1]);
					std::vector<int> ints;
					ints.resize(sizes[2], 0);
					std::vector<double> doubles;
					doubles.resize(sizes[3],0.0);
					char *buffer = new char[sizes[0]];
					MPI_Recv((void *) buffer, sizes[0], MPI_PACKED, pold, 0, MPI_COMM_WORLD, &mpi_status);

					int position = 0;
					MPI_Unpack(buffer, sizes[0], &position, &(string_buffer[0]), sizes[1],
						MPI_CHAR, MPI_COMM_WORLD);
					MPI_Unpack(buffer, sizes[0], &position, &(ints[0]), sizes[2],
						MPI_INT, MPI_COMM_WORLD);
					MPI_Unpack(buffer, sizes[0], &position, &(doubles[0]), sizes[3],
						MPI_DOUBLE, MPI_COMM_WORLD);

					Dictionary dictionary(string_buffer);
					IPhreeqcPhast * phast_iphreeqc_worker = this->workers[0];
					serial.Deserialize(*phast_iphreeqc_worker->Get_PhreeqcPtr(), dictionary, ints, doubles);
					delete [] buffer;
				}
				transfers++;
			}
			catch (...)
			{
				r_vector[0] = 1;
			}

			// Delete cells in old
			if (this->mpi_myself == pold && r_vector[0] == 0)
			{
				std::ostringstream del;
				del << "DELETE; -cell\n";
				for (size_t i = 2; i < tp_it->second.size(); i++)
				{
					del << tp_it->second[i] << "\n";

				}
				try
				{
					int status = phast_iphreeqc_worker->RunString(del.str().c_str());
					if (status != 0)
					{
						this->ErrorMessage(phast_iphreeqc_worker->GetErrorString());
					}
					this->ErrorHandler(PhreeqcRM::Int2IrmResult(status, false), "RunString");
				}
				catch (...)
				{
					r_vector[0] = 1;
				}
			}

			// Also need to tranfer UZ
			if (this->partition_uz_solids)
			{
				std::ostringstream uz_dump;
				if (this->mpi_myself == pold)
				{
					for (size_t i = 2; i < tp_it->second.size(); i++)
					{
						int k = tp_it->second[i];
						phast_iphreeqc_worker->uz_bin.Remove_Solution(k);
						phast_iphreeqc_worker->uz_bin.dump_raw(uz_dump, k, 0);
						phast_iphreeqc_worker->uz_bin.Remove(k);
					}
				}
				try
				{
					this->TransferCellsUZ(uz_dump, pold, pnew);
				}
				catch (...)
				{
					r_vector[0] = 1;
				}
			}

			//The gather is sometimes slow for some reason
			//this->HandleErrorsInternal(r_vector);
			if (r_vector[0] != 0)
				throw PhreeqcRMStop();
		}
		for (int i = 0; i < this->mpi_tasks; i++)
		{
			start_cell[i] = start_cell_new[i];
			end_cell[i] = end_cell_new[i];
		}
		if (this->mpi_myself == 0)
		{
			//std::cerr << "          Cells shifted between processes     " << change << "\n";
			std::ostringstream msg;
			msg << "          Cells shifted between processes     " << change << "\n";
			this->ScreenMessage(msg.str().c_str());
		}

		if (change > 0)
		{
			ScatterNchem(print_chem_mask_root, print_chem_mask_worker);
			ScatterNchem(density_root, density_worker);
			//ScatterNchem(tempc_root, tempc_worker);
			ScatterNchem(porosity_root, porosity_worker);
			ScatterNchem(rv_root, rv_worker);
			//ScatterNchem(pressure_root, pressure_worker);
			ScatterNchem(saturation_root, saturation_worker);
			if (partition_uz_solids)
			{
				ScatterNchem(old_saturation_root, old_saturation_worker);
			}
		}

	}
	catch (...)
	{
		this->ErrorHandler(IRM_FAIL, "PhreeqcRM::RebalanceLoadPerCell");
	}
}
#else
/* ---------------------------------------------------------------------- */
void
PhreeqcRM::RebalanceLoadPerCell(void)
/* ---------------------------------------------------------------------- */
{
	// Throws on error
	if (this->mpi_tasks <= 1) return;
	if (this->mpi_tasks > count_chemistry) return;
#include <time.h>

	// vectors for each cell (count_chem)
	std::vector<double> recv_cell_times, normalized_cell_times;
	recv_cell_times.resize(this->count_chemistry);

	// vectors for each process (mpi_tasks)
	std::vector<double> standard_time, task_fraction, task_time;

	// Assume homogeneous cluster for now
	if (mpi_myself == 0)
	{
		double tasks_total = 0;
		for (size_t i = 0; i < (size_t) mpi_tasks; i++)
		{
			standard_time.push_back(this->standard_task_vector[i]);   // slower is bigger number
			//standard_time.push_back(1.0);                           // homogeneous
			tasks_total += 1.0 / standard_time[i];
		}

		for (size_t i = 0; i < (size_t) mpi_tasks; i++)
		{
			task_fraction.push_back((1.0 / standard_time[i]) / tasks_total);
		}
	}
	// Collect times
	IPhreeqcPhast * phast_iphreeqc_worker = this->workers[0];
	// manager
	if (mpi_myself == 0)
	{
		recv_cell_times.insert(recv_cell_times.begin(),
			phast_iphreeqc_worker->Get_cell_clock_times().begin(),
			phast_iphreeqc_worker->Get_cell_clock_times().end());
	}

	// workers
	for (int i = 1; i < mpi_tasks; i++)
	{
		int n = end_cell[i] - start_cell[i] + 1;
		if (mpi_myself == i)
		{
			MPI_Send(&phast_iphreeqc_worker->Get_cell_clock_times().front(), n, MPI_DOUBLE, 0, 0, phreeqcrm_comm);
		}
		if (mpi_myself == 0)
		{
			MPI_Status mpi_status;
			MPI_Recv((void *) &recv_cell_times[start_cell[i]], n, MPI_DOUBLE, i, 0, phreeqcrm_comm, &mpi_status);
		}
	}
	phast_iphreeqc_worker->Get_cell_clock_times().clear();
	// Root normalizes times, calculates efficiency, rebalances work
	double normalized_total_time = 0;
	double max_task_time = 0;
	// working space
	std::vector<int> start_cell_new;
	std::vector<int> end_cell_new;
	start_cell_new.resize(mpi_tasks, 0);
	end_cell_new.resize(mpi_tasks, 0);

	if (mpi_myself == 0)
	{
		// Normalize times
		max_task_time = 0;
		for (size_t i = 0; i < (size_t) mpi_tasks; i++)
		{
			double task_sum = 0;
			// normalize cell_times with standard_time
			for (size_t j = (size_t) start_cell[i]; j <= (size_t) end_cell[i]; j++)
			{
				task_sum += recv_cell_times[j];
				normalized_cell_times.push_back(recv_cell_times[j]/standard_time[i]);
				normalized_total_time += normalized_cell_times.back();
			}
			task_time.push_back(task_sum);
			max_task_time = (task_sum > max_task_time) ? task_sum : max_task_time;
		}

		// calculate efficiency
		double efficiency = 0;
		for (size_t i = 0; i < (size_t) mpi_tasks; i++)
		{
			efficiency += task_time[i] / max_task_time * task_fraction[i];
		}
		{
			//std::cerr << "          Estimated efficiency of chemistry without communication: " <<
			//		   (float) (100. * efficiency) << "\n";
			std::ostringstream msg;
			msg << "          Estimated efficiency of chemistry without communication: " <<
					   (float) (100. * efficiency) << "\n";
			this->ScreenMessage(msg.str().c_str());
		}

		// Split up work
		double f_low, f_high;
		f_high = 1 + 0.5 / ((double) mpi_tasks);
		f_low = 1;
		int j = 0;
		for (size_t i = 0; i < (size_t) mpi_tasks - 1; i++)
		{
			if (i > 0)
			{
				start_cell_new[i] = end_cell_new[i - 1] + 1;
			}
			double sum_work = 0;
			double temp_sum_work = 0;
			bool next = true;
			while (next)
			{
				temp_sum_work += normalized_cell_times[j] / normalized_total_time;
				if ((temp_sum_work < task_fraction[i]) && (((size_t) count_chemistry - j) > (size_t) (mpi_tasks - i)))
				{
					sum_work = temp_sum_work;
					j++;
					next = true;
				}
				else
				{
					if (j == start_cell_new[i])
					{
						end_cell_new[i] = j;
						j++;
					}
					else
					{
						end_cell_new[i] = j - 1;
					}
					next = false;
				}
			}
		}
		assert(j < count_chemistry);
		assert(mpi_tasks > 1);
		start_cell_new[mpi_tasks - 1] = end_cell_new[mpi_tasks - 2] + 1;
		end_cell_new[mpi_tasks - 1] = count_chemistry - 1;

		if (efficiency > 0.95)
		{
			for (int i = 0; i < this->mpi_tasks; i++)
			{
				start_cell_new[i] = start_cell[i];
				end_cell_new[i] = end_cell[i];
			}
		}
		else
		{
			for (size_t i = 0; i < (size_t) this->mpi_tasks - 1; i++)
			{
				int	icells;
				icells = (int) (((double) (end_cell_new[i] - end_cell[i])) * (this->rebalance_fraction) );
				if (icells == 0)
				{
					icells = end_cell_new[i] - end_cell[i];
				}
				end_cell_new[i] = end_cell[i] + icells;
				start_cell_new[i + 1] = end_cell_new[i] + 1;
			}
		}

	}

	/*
	 *   Broadcast new subcolumns
	 */

	MPI_Bcast((void *) &start_cell_new.front(), mpi_tasks, MPI_INT, 0, phreeqcrm_comm);
	MPI_Bcast((void *) &end_cell_new.front(), mpi_tasks, MPI_INT, 0, phreeqcrm_comm);

	/*
	 *   Redefine columns
	 */
	int nnew = 0;
	int old = 0;
	int change = 0;

			std::map< std::string, std::vector<int> > transfer_pair;
	for (int k = 0; k < this->count_chemistry; k++)
	{
		while (k > end_cell[old])
		{
			old++;
		}
		while (k > end_cell_new[nnew])
		{
			nnew++;
		}

		if (old == nnew)
			continue;
		change++;

		// Need to send cell from old task to nnew task
		std::ostringstream key;
		key << old << "#" << nnew;
		std::map< std::string, std::vector<int> >::iterator tp_it = transfer_pair.find(key.str());
		if (tp_it == transfer_pair.end())
		{
			std::vector<int> v;
			v.push_back(old);
			v.push_back(nnew);
			transfer_pair[key.str()] = v;
		}
		transfer_pair[key.str()].push_back(k);
	}

	// Transfer cells
	int transfers = 0;
	try
	{
		std::map< std::string, std::vector<int> >::iterator tp_it = transfer_pair.begin();
		std::vector<int> r_vector;
		r_vector.push_back(IRM_OK);
		for ( ; tp_it != transfer_pair.end(); tp_it++)
		{
			cxxStorageBin t_bin;
			int pold = tp_it->second[0];
			int pnew = tp_it->second[1];
			if (this->mpi_myself == pold)
			{
				for (size_t i = 2; i < tp_it->second.size(); i++)
				{
					int k = tp_it->second[i];
					phast_iphreeqc_worker->Get_PhreeqcPtr()->phreeqc2cxxStorageBin(t_bin, k);
				}
			}
			transfers++;
			try
			{
				this->TransferCells(t_bin, pold, pnew);
			}
			catch (...)
			{
				r_vector[0] = 1;
			}

			// Delete cells in old
			if (this->mpi_myself == pold && r_vector[0] == 0)
			{
				std::ostringstream del;
				del << "DELETE; -cell\n";
				for (size_t i = 2; i < tp_it->second.size(); i++)
				{
					del << tp_it->second[i] << "\n";

				}
				try
				{
					int status = phast_iphreeqc_worker->RunString(del.str().c_str());
					if (status != 0)
					{
						this->ErrorMessage(phast_iphreeqc_worker->GetErrorString());
					}
					this->ErrorHandler(PhreeqcRM::Int2IrmResult(status, false), "RunString");
				}
				catch (...)
				{
					r_vector[0] = 1;
				}
			}

			// Also need to tranfer UZ
			if (this->partition_uz_solids)
			{
				std::ostringstream uz_dump;
				if (this->mpi_myself == pold)
				{
					for (size_t i = 2; i < tp_it->second.size(); i++)
					{
						int k = tp_it->second[i];
						phast_iphreeqc_worker->uz_bin.Remove_Solution(k);
						phast_iphreeqc_worker->uz_bin.dump_raw(uz_dump, k, 0);
						phast_iphreeqc_worker->uz_bin.Remove(k);
					}
				}
				try
				{
					this->TransferCellsUZ(uz_dump, pold, pnew);
				}
				catch (...)
				{
					r_vector[0] = 1;
				}
			}

			//The gather is sometimes slow for some reason
			//this->HandleErrorsInternal(r_vector);
			if (r_vector[0] != 0)
				throw PhreeqcRMStop();
		}
		for (int i = 0; i < this->mpi_tasks; i++)
		{
			start_cell[i] = start_cell_new[i];
			end_cell[i] = end_cell_new[i];
		}
		if (this->mpi_myself == 0)
		{
			//std::cerr << "          Cells shifted between processes     " << change << "\n";
			std::ostringstream msg;
			msg << "          Cells shifted between processes     " << change << "\n";
			this->ScreenMessage(msg.str().c_str());
		}

		if (change > 0)
		{
			ScatterNchem(print_chem_mask_root, print_chem_mask_worker);
			ScatterNchem(density_root, density_worker);
			//ScatterNchem(tempc_root, tempc_worker);
			ScatterNchem(porosity_root, porosity_worker);
			ScatterNchem(rv_root, rv_worker);
			//ScatterNchem(pressure_root, pressure_worker);
			ScatterNchem(saturation_root, saturation_worker);
			if (partition_uz_solids)
			{
				ScatterNchem(old_saturation_root, old_saturation_worker);
			}
		}

	}
	catch (...)
	{
		this->ErrorHandler(IRM_FAIL, "PhreeqcRM::RebalanceLoadPerCell");
	}
}
#endif
#else
/* ---------------------------------------------------------------------- */
void
PhreeqcRM::RebalanceLoadPerCell(void)
/* ---------------------------------------------------------------------- */
{
	// Threaded version
	if (this->nthreads <= 1) return;
	if (this->nthreads > count_chemistry) return;
#include <time.h>

	// vectors for each cell (count_chem)
	std::vector<double> recv_cell_times, normalized_cell_times;

	// vectors for each process (mpi_tasks)
	std::vector<double> standard_time, task_fraction, task_time;

	// Assume homogeneous cluster for now
	double tasks_total = 0;
	for (size_t i = 0; i < (size_t) this->nthreads; i++)
	{
		standard_time.push_back(1.0);   // For heterogeneous cluster, need times for a standard task here
		tasks_total += 1.0 / standard_time[i];
	}

	for (size_t i = 0; i < (size_t) this->nthreads; i++)
	{
		task_fraction.push_back((1.0 / standard_time[i]) / tasks_total);
	}


	for (size_t i = 0; i < (size_t) this->nthreads; i++)
	{
		IPhreeqcPhast * phast_iphreeqc_worker = this->workers[i];
		std::vector<double>::const_iterator cit;
		for (cit = phast_iphreeqc_worker->Get_cell_clock_times().begin();
			cit != phast_iphreeqc_worker->Get_cell_clock_times().end();
			cit++)
		{
			recv_cell_times.push_back(*cit);
		}
		phast_iphreeqc_worker->Get_cell_clock_times().clear();
	}
	// Root normalizes times, calculates efficiency, rebalances work
	double normalized_total_time = 0;
	double max_task_time = 0;
	// working space
	std::vector<int> start_cell_new;
	std::vector<int> end_cell_new;
	start_cell_new.resize(this->nthreads, 0);
	end_cell_new.resize(this->nthreads, 0);


	// Normalize times
	max_task_time = 0;
	for (size_t i = 0; i < (size_t) this->nthreads; i++)
	{
		double task_sum = 0;
		// normalize cell_times with standard_time
		for (size_t j = (size_t) start_cell[i]; j <= (size_t) end_cell[i]; j++)
		{
			task_sum += recv_cell_times[j];
			normalized_cell_times.push_back(recv_cell_times[j]/standard_time[i]);
			normalized_total_time += normalized_cell_times.back();
		}
		task_time.push_back(task_sum);
		max_task_time = (task_sum > max_task_time) ? task_sum : max_task_time;
	}

	// calculate efficiency
	double efficiency = 0;
	for (size_t i = 0; i < (size_t) this->nthreads; i++)
	{
		efficiency += task_time[i] / max_task_time * task_fraction[i];
	}
	{
			//std::cerr << "          Estimated efficiency of chemistry without communication: " <<
			//	(float) (100. * efficiency) << "\n";
			std::ostringstream msg;
			msg << "          Estimated efficiency of chemistry without communication: " <<
				(float) (100. * efficiency) << "\n";
			this->ScreenMessage(msg.str().c_str());
	}

	// Split up work
	//double f_low, f_high;
	//f_high = 1 + 0.5 / ((double) this->nthreads);
	//f_low = 1;
	int j = 0;
	for (size_t i = 0; i < (size_t) this->nthreads - 1; i++)
	{
		if (i > 0)
		{
			start_cell_new[i] = end_cell_new[i - 1] + 1;
		}
		double temp_sum_work = 0;
		bool next = true;
		while (next)
		{
			temp_sum_work += normalized_cell_times[j] / normalized_total_time;
			if ((temp_sum_work < task_fraction[i]) && ((count_chemistry - (int) j) > (this->nthreads - (int) i)))
			{
				//sum_work = temp_sum_work;
				j++;
				next = true;
			}
			else
			{
				if (j == start_cell_new[i])
				{
					end_cell_new[i] = j;
					j++;
				}
				else
				{
					end_cell_new[i] = j - 1;
				}
				next = false;
			}
		}
	}
	assert(j < count_chemistry);
	assert(this->nthreads > 1);
	start_cell_new[this->nthreads - 1] = end_cell_new[this->nthreads - 2] + 1;
	end_cell_new[this->nthreads - 1] = count_chemistry - 1;

	if (efficiency > 0.95)
	{
			for (int i = 0; i < this->nthreads; i++)
			{
				start_cell_new[i] = start_cell[i];
				end_cell_new[i] = end_cell[i];
			}
	}
	else
	{
		for (size_t i = 0; i < (size_t) this->nthreads - 1; i++)
		{
			int	icells;
			icells = (int) (((double) (end_cell_new[i] - end_cell[i])) * (this->rebalance_fraction) );
			if (icells == 0)
			{
				icells = end_cell_new[i] - end_cell[i];
			}
			end_cell_new[i] = end_cell[i] + icells;
			start_cell_new[i + 1] = end_cell_new[i] + 1;
		}
	}


	/*
	 *   Redefine columns
	 */
	int nnew = 0;
	int old = 0;
	int change = 0;
	try
	{
		for (int k = 0; k < this->count_chemistry; k++)
		{
			int i = k;
			int iphrq = i;			/* iphrq is 1 to count_chem */
			while (k > end_cell[old])
			{
				old++;
			}
			while (k > end_cell_new[nnew])
			{
				nnew++;
			}

			if (old == nnew)
				continue;
			change++;
			IPhreeqcPhast * old_worker = this->workers[old];
			IPhreeqcPhast * new_worker = this->workers[nnew];
			cxxStorageBin temp_bin;
			old_worker->Get_PhreeqcPtr()->phreeqc2cxxStorageBin(temp_bin, iphrq);
			new_worker->Get_PhreeqcPtr()->cxxStorageBin2phreeqc(temp_bin, iphrq);
			std::ostringstream del;
			del << "DELETE; -cell " << iphrq << "\n";
			int status = old_worker->RunString(del.str().c_str());
			if (status != 0)
			{
				this->ErrorMessage(old_worker->GetErrorString());
			}
			this->ErrorHandler(PhreeqcRM::Int2IrmResult(status, false), "RunString in RebalanceLoadPerCell");

			// Also need to tranfer UZ
			if (this->partition_uz_solids)
			{
				new_worker->uz_bin.Add(old_worker->uz_bin, iphrq);
				old_worker->uz_bin.Remove(iphrq);
			}
		}

		for (int i = 0; i < this->nthreads; i++)
		{
			start_cell[i] = start_cell_new[i];
			end_cell[i] = end_cell_new[i];
			IPhreeqcPhast * worker = this->workers[i];
			worker->Set_start_cell(start_cell_new[i]);
			worker->Set_end_cell(end_cell_new[i]);
		}
		{
			//std::cerr << "          Cells shifted between threads     " << change << "\n";
			std::ostringstream msg;
			msg << "          Cells shifted between threads     " << change << "\n";
			this->ScreenMessage(msg.str().c_str());
		}
	}
	catch (...)
	{
		this->ErrorHandler(IRM_FAIL, "PhreeqcRM::RebalanceLoadPerCell");
	}
}
#endif

/* ---------------------------------------------------------------------- */
inline IRM_RESULT
PhreeqcRM::ReturnHandler(IRM_RESULT result, const std::string & e_string)
/* ---------------------------------------------------------------------- */
{
	if (result < 0)
	{
		{
			this->DecodeError(result);
			this->ErrorMessage(e_string);
			std::ostringstream flush;
			flush << std::endl;
			this->ErrorMessage(flush.str(), false);
		}
		switch (this->error_handler_mode)
		{
		case 0:
			return result;
			break;
		case 1:
			throw PhreeqcRMStop();
			break;
		case 2:
			exit(result);
			break;
		default:
			return result;
			break;
		}
	}
	return result;
}
#ifdef USE_MPI
/* ---------------------------------------------------------------------- */
IRM_RESULT
PhreeqcRM::RunCells()
/* ---------------------------------------------------------------------- */
{
/*
 *   Routine runs reactions for each cell
 */
	this->phreeqcrm_error_string.clear();
	if (mpi_myself == 0)
	{
		if (IthConcentrationSet.size() > 0)
		{
			if (IthConcentrationSet.size() != this->GetComponentCount())
			{
				this->ErrorMessage("You must call SetIthConcentration for every component before you call Run_Cells.");
				throw PhreeqcRMStop();
			}
			SetConcentrations(IthCurrentConcentrations);
		}
		if (IthSpeciesConcentrationSet.size() > 0)
		{
			if (IthSpeciesConcentrationSet.size() != this->GetSpeciesCount())
			{
				this->ErrorMessage("You must call SetIthSpeciesConcentration for every species before you call Run_Cells.");
				throw PhreeqcRMStop();
			}
			SpeciesConcentrations2Module(IthCurrentSpeciesConcentrations);
		}
	}
	if (mpi_myself == 0)
	{
		int method = METHOD_RUNCELLS;
		MPI_Bcast(&method, 1, MPI_INT, 0, phreeqcrm_comm);
	}
	// check that all solutions are defined
	if (this->need_error_check)
	{

		this->need_error_check = false;
		try
		{
			CheckCells();
		}
		catch(...)
		{
			MPI_Barrier(phreeqcrm_comm);
			return this->ReturnHandler(IRM_FAIL, "PhreeqcRM::RunCells");
		}
	}
	IRM_RESULT return_value = IRM_OK;
	/*
	*   Update solution compositions in sz_bin
	*/

	IPhreeqcPhast * phast_iphreeqc_worker = this->workers[0];
	phast_iphreeqc_worker->PhreeqcPtr->Set_run_cells_one_step(true);
	if (phast_iphreeqc_worker->Get_out_stream())
	{
		delete phast_iphreeqc_worker->Get_out_stream();
	}
	phast_iphreeqc_worker->Set_out_stream(new std::ostringstream);
	if (phast_iphreeqc_worker->Get_punch_stream())
	{
		delete phast_iphreeqc_worker->Get_punch_stream();
	}
	phast_iphreeqc_worker->Set_punch_stream(new std::ostringstream);

	// Run cells in each process
	std::vector<int> r_vector;
	r_vector.resize(1);
	r_vector[0] = RunCellsThread(0);
	if (this->partition_uz_solids)
	{
		old_saturation_worker = saturation_worker;
		if (mpi_myself == 0)
		{
			old_saturation_root = saturation_root;
		}
	}

	std::vector<char> char_buffer;
	std::vector<double> double_buffer;
	for (int n = 0; n < this->mpi_tasks; n++)
	{

		// write output results
		if (this->print_chemistry_on[0])
		{
			// Need to transfer output stream to root and print
			if (this->mpi_myself == n)
			{
				if (n == 0)
				{
					this->OutputMessage(this->workers[0]->Get_out_stream()->str().c_str());
					delete this->workers[0]->Get_out_stream();
					this->workers[0]->Set_out_stream(NULL);
				}
				else
				{
					int size = (int) this->workers[0]->Get_out_stream()->str().size();
					MPI_Send(&size, 1, MPI_INT, 0, 0, phreeqcrm_comm);
					MPI_Send((void *) this->workers[0]->Get_out_stream()->str().c_str(), size, MPI_CHAR, 0, 0, phreeqcrm_comm);
					delete this->workers[0]->Get_out_stream();
					this->workers[0]->Set_out_stream(NULL);
				}
			}
			else if (this->mpi_myself == 0)
			{
				MPI_Status mpi_status;
				int size;
				MPI_Recv(&size, 1, MPI_INT, n, 0, phreeqcrm_comm, &mpi_status);
				char_buffer.resize(size + 1);
				MPI_Recv((void *) &char_buffer.front(), size, MPI_CHAR, n, 0, phreeqcrm_comm, &mpi_status);
				char_buffer[size] = '\0';
				this->OutputMessage(&char_buffer.front());
			}
		}
	}

	// Count errors and write error messages
	try
	{
		HandleErrorsInternal(r_vector);

	// Debugging selected output
#if !defined(NDEBUG)
		this->CheckSelectedOutput();
#endif
		// Rebalance load
		double t0 = (double) CLOCK();
		this->RebalanceLoad();
		MPI_Barrier(this->phreeqcrm_comm);
		if (mpi_myself == 0 && mpi_tasks > 1)
		{
			std::ostringstream msg;
			msg << "          Time rebalancing load             " << ((double) CLOCK() - t0) << "\n";
			this->ScreenMessage(msg.str().c_str());
		}
	}
	catch (...)
	{
		return_value = IRM_FAIL;
	}
	if (mpi_myself == 0)
	{
		GetConcentrations(this->CurrentConcentrations);
		this->IthConcentrationSet.clear();
		this->IthSpeciesConcentrationSet.clear();
		if (this->species_save_on)
		{
			GetSpeciesConcentrations(this->CurrentSpeciesConcentrations);
		}
		this->ClearBMISelectedOutput();
	}
	return this->ReturnHandler(return_value, "PhreeqcRM::RunCells");
}
#else
/* ---------------------------------------------------------------------- */
IRM_RESULT
PhreeqcRM::RunCells()
/* ---------------------------------------------------------------------- */
{
/*
 *   Routine takes mass fractions from HST, equilibrates each cell,
 *   and returns new mass fractions to HST
 */

/*
 *   Update solution compositions in sz_bin
 */
	this->phreeqcrm_error_string.clear();
	if (IthConcentrationSet.size() > 0)
	{
		if (IthConcentrationSet.size() != this->GetComponentCount())
		{
			this->ErrorMessage("You must call SetIthConcentration for every component before you call Run_Cells.");
			throw PhreeqcRMStop();
		}
		SetConcentrations(IthCurrentConcentrations);
	}
	if (IthSpeciesConcentrationSet.size() > 0)
	{
		if (IthSpeciesConcentrationSet.size() != this->GetSpeciesCount())
		{
			this->ErrorMessage("You must call SetIthSpeciesConcentration for every species before you call Run_Cells.");
			throw PhreeqcRMStop();
		}
		SpeciesConcentrations2Module(IthCurrentSpeciesConcentrations);
	}
	// check that all solutions are defined
	if (this->need_error_check)
	{
		this->need_error_check = false;
		try
		{
			CheckCells();
		}
		catch(...)
		{
			return this->ReturnHandler(IRM_FAIL, "PhreeqcRM::RunCells");
		}
	}
	IRM_RESULT return_value = IRM_OK;
	try
	{
		for (int n = 0; n < this->nthreads; n++)
		{
			IPhreeqcPhast * phast_iphreeqc_worker = this->workers[n];
			phast_iphreeqc_worker->PhreeqcPtr->Set_run_cells_one_step(true);
			if (phast_iphreeqc_worker->Get_out_stream())
			{
				delete phast_iphreeqc_worker->Get_out_stream();
			}
			phast_iphreeqc_worker->Set_out_stream(new std::ostringstream);
			if (phast_iphreeqc_worker->Get_punch_stream())
			{
				delete phast_iphreeqc_worker->Get_punch_stream();
			}
			phast_iphreeqc_worker->Set_punch_stream(new std::ostringstream);
		}
		std::vector < int > r_vector;
		r_vector.resize(this->nthreads);
#ifdef USE_OPENMP
		omp_set_num_threads(this->nthreads);
#pragma omp parallel
#pragma omp for
#endif
		for (int n = 0; n < this->nthreads; n++)
		{
			r_vector[n] = RunCellsThread(n);
		}
		if (this->partition_uz_solids)
		{
			old_saturation_root = saturation_root;
		}


		// write output results
		for (int n = 0; n < this->nthreads; n++)
		{
			if (this->print_chemistry_on[0])
			{
				this->OutputMessage(this->workers[n]->Get_out_stream()->str().c_str());
			}
			delete this->workers[n]->Get_out_stream();
			this->workers[n]->Set_out_stream(NULL);
		}

		// Count errors and write error messages
		HandleErrorsInternal(r_vector);

#if !defined(NDEBUG)
		this->CheckSelectedOutput();
#endif
		// Rebalance load
		double t0 = CLOCK();
		this->RebalanceLoad();
		if (mpi_myself == 0 && nthreads > 1)
		{
			std::ostringstream msg;
			msg << "          Time rebalancing load             " << ((double) CLOCK() - t0) << "\n";
			this->ScreenMessage(msg.str().c_str());
		}
	}
	catch (...)
	{
		return_value = IRM_FAIL;
	}
	GetConcentrations(this->CurrentConcentrations);
	this->IthConcentrationSet.clear();
	this->IthSpeciesConcentrationSet.clear();
	if (this->species_save_on)
	{
		GetSpeciesConcentrations(this->CurrentSpeciesConcentrations);
	}
	this->ClearBMISelectedOutput();
	return this->ReturnHandler(return_value, "PhreeqcRM::RunCells");
}
#endif
/* ---------------------------------------------------------------------- */
IRM_RESULT
PhreeqcRM::RunCellsThreadNoPrint(int n)
/* ---------------------------------------------------------------------- */
{

	/*
	*   This routine equilibrates each cell for the given thread
	*   when there is no printout (SetChemistryOn(false)).
	*/
	IPhreeqcPhast *phast_iphreeqc_worker = this->GetWorkers()[n];

	// selected output IPhreeqcPhast
	phast_iphreeqc_worker->CSelectedOutputMap.clear();
	// Make a dummy run to fill in new CSelectedOutputMap
	
	if (selected_output_on)
	{
		std::ostringstream input;
		input << "PRINT; -selected_output true\n";
		int next = phast_iphreeqc_worker->PhreeqcPtr->next_user_number(Keywords::KEY_SOLUTION);
		input << "SOLUTION " << next << ";";
		if (phast_iphreeqc_worker->PhreeqcPtr->llnl_temp.size() > 0)
		{
			input << "-temp " << phast_iphreeqc_worker->PhreeqcPtr->llnl_temp[0] << ";";
		}
		input << "DELETE; -solution " << next << "\n";
		if (phast_iphreeqc_worker->RunString(input.str().c_str()) < 0)
		{
			this->ErrorMessage(phast_iphreeqc_worker->GetErrorString());
			throw PhreeqcRMStop();
		}
		std::map< int, CSelectedOutput* >::iterator it = phast_iphreeqc_worker->SelectedOutputMap.begin();
		for ( ; it != phast_iphreeqc_worker->SelectedOutputMap.end(); it++)
		{
			int iso = it->first;
			// Add new item to CSelectedOutputMap
			CSelectedOutput cso;
			// Fill in columns
			phast_iphreeqc_worker->SetCurrentSelectedOutputUserNumber(iso);
			int columns = phast_iphreeqc_worker->GetSelectedOutputColumnCount();
			for (int i = 0; i < columns; i++)
			{
				VAR pvar, pvar1;
				VarInit(&pvar);
				VarInit(&pvar1);
				phast_iphreeqc_worker->GetSelectedOutputValue(0, i, &pvar);
				cso.PushBack(pvar.sVal, pvar1);
				VarClear(&pvar);
				VarClear(&pvar1);
			}
			phast_iphreeqc_worker->CSelectedOutputMap[iso] = cso;
		}
	}
	// Arrays for tranferring selected output
	std::vector<int> types;
	std::vector<long> longs;
	std::vector<double> doubles;
	std::string strings;

	// Do not write to files from phreeqc, run_cells writes files
	phast_iphreeqc_worker->SetLogFileOn(false);
	phast_iphreeqc_worker->SetSelectedOutputFileOn(false);
	phast_iphreeqc_worker->SetDumpFileOn(false);
	phast_iphreeqc_worker->SetDumpStringOn(false);
	phast_iphreeqc_worker->SetOutputFileOn(false);
	//phast_iphreeqc_worker->SetErrorFileOn(false);
	phast_iphreeqc_worker->SetOutputStringOn(false);
#ifdef USE_MPI
	int start = this->start_cell[this->mpi_myself];
	int end = this->end_cell[this->mpi_myself];
#else
	int start = this->start_cell[n];
	int end = this->end_cell[n];
#endif
	//phast_iphreeqc_worker->Get_cell_clock_times().clear();

	if (this->rebalance_by_cell)
	{
		if (phast_iphreeqc_worker->Get_cell_clock_times().size() == 0)
		{
			phast_iphreeqc_worker->Get_cell_clock_times().resize(end - start + 1, 0.0);
		}
	}
	// Make list of cells with saturation > 0.0
	std::ostringstream soln_list;
	int count_active = 0;
	int range_start = -1, range_end = -1;

	// Find first active cell
	for (int i = start; i <= end; i++)
	{
#ifdef USE_MPI
		int j = i; // not used
		double sat = saturation_worker[i - start];
#else
		int j = backward_mapping[i][0];			/* j is nxyz number */
		double sat = saturation_root[j];
#endif
		if (sat > 1e-6)
		{
			range_start = i;
			range_end = i;
			count_active++;
			if (this->partition_uz_solids)
			{
				this->PartitionUZ(n, i, j, sat);
			}
			break;
		}
	}
	if (count_active > 0)
	{
		int first_active = range_start;
		for (int i = first_active + 1; i <= end; i++)
		{		  					                /* i is count_chem number */
#ifdef USE_MPI
			int j = i; // not used
		double sat = saturation_worker[i - start];
#else
		int j = backward_mapping[i][0];			/* j is nxyz number */
		double sat = saturation_root[j];
#endif
			if (sat > 1e-6)
			{
				count_active++;
				if (i == range_end + 1)
				{
					range_end++;
				}
				else
				{
					if (range_start == range_end)
					{
						soln_list << range_start << "\n";
					}
					else
					{
						soln_list << range_start << "-" << range_end << "\n";
					}
					range_start = i;
					range_end = i;
				}
				// partition solids between UZ and SZ
				if (this->partition_uz_solids)
				{
					this->PartitionUZ(n, i, j, sat);
				}
			}
		}
		if (range_start == range_end)
		{
			soln_list << range_start << "\n";
		}
		else
		{
			soln_list << range_start << "-" << range_end << "\n";
		}
	}

	double t0 = (double) CLOCK();
	if (count_active > 0)
	{
		std::ostringstream input;
		if(this->selected_output_on)
		{
			input << "PRINT; -selected_output true\n";
		}
		else
		{
			input << "PRINT; -selected_output false\n";
		}
		input << "RUN_CELLS\n";
		input << "  -start_time " << (this->time - this->time_step) << "\n";
		input << "  -time_step  " << this->time_step << "\n";
		input << "  -cells      " << soln_list.str();
		input << "END" << "\n";

		if (phast_iphreeqc_worker->RunString(input.str().c_str()) != 0)
		{
			this->ErrorMessage(phast_iphreeqc_worker->GetErrorString());
			throw PhreeqcRMStop();
		}
	}
	double t_elapsed = (double) CLOCK() - t0;

	// Save selected output data
	if (this->selected_output_on)
	{
		// Add selected output values to IPhreeqcPhast CSelectedOutputMap's
		std::map< int, CSelectedOutput* >::iterator it = phast_iphreeqc_worker->SelectedOutputMap.begin();
		for ( ; it != phast_iphreeqc_worker->SelectedOutputMap.end(); it++)
		{
			int n_user = it->first;
			std::map< int, CSelectedOutput >::iterator ipp_it = phast_iphreeqc_worker->CSelectedOutputMap.find(n_user);
			if (ipp_it == phast_iphreeqc_worker->CSelectedOutputMap.end())
			{
				std::cerr << "Item not found in CSelectedOutputMap" << std::endl;
				throw PhreeqcRMStop();
			}
			int counter = 0;
			for (int i = start; i <= end; i++)
			{							                /* i is count_chem number */
#ifdef USE_MPI
				double sat = saturation_worker[i - start];
#else
				int j = backward_mapping[i][0];			/* j is nxyz number */
				double sat = saturation_root[j];
#endif
				if (sat > 1e-6)
				{
					types.clear();
					longs.clear();
					doubles.clear();
					strings.clear();
					it->second->Serialize(counter, types, longs, doubles, strings);
					ipp_it->second.DeSerialize(types, longs, doubles, strings);
					counter++;
				}
				else
				{
					ipp_it->second.EndRow();
				}
				}
			}
			}
	// Set cell_clock_times
	if (this->rebalance_by_cell)
	{
		for (int i = start; i <= end; i++)
		{							                /* i is count_chem number */
#ifdef USE_MPI
		double sat = saturation_worker[i - start];
#else
			int j = backward_mapping[i][0];			/* j is nxyz number */
			double sat = saturation_root[j];
#endif
			if (sat > 1e-6 )
			{
				phast_iphreeqc_worker->Get_cell_clock_times()[i - start] += t_elapsed / (double) count_active;
			}
		}
	}

	return IRM_OK;
}
/* ---------------------------------------------------------------------- */
IRM_RESULT
PhreeqcRM::RunCellsThread(int n)
/* ---------------------------------------------------------------------- */
{
	/*
	*   Then this routine equilibrates each cell for the given thread
	*/

	/*
	*   Update solution compositions
	*/
	double t0 = (double) CLOCK();

	int i, j;
	IPhreeqcPhast *phast_iphreeqc_worker = this->GetWorkers()[n];
	try
	{
		// Partition solids, if necessary
		if (this->partition_uz_solids)
		{
#ifdef USE_MPI
			int start = this->start_cell[this->mpi_myself];
			int end = this->end_cell[this->mpi_myself];
			int iworker = 0;
#else
			int start = this->start_cell[n];
			int end = this->end_cell[n];
			int iworker = n;
#endif
			// run the cells
			for (i = start; i <= end; i++)
			{							            /* i is count_chem number */
#ifdef USE_MPI
				j = i; // unused
				double sat = saturation_worker[i - start];
#else
				j = backward_mapping[i][0];			/* j is nxyz number */
				double sat = saturation_root[j];
#endif
				this->PartitionUZ(iworker, i, j, sat);
			}
		}

		// Find the print flag
		bool pr_chemistry_on;
		if (n < this->nthreads)
		{
			pr_chemistry_on = print_chemistry_on[0];
		}
		else if (n == this->nthreads)
		{
			pr_chemistry_on = print_chemistry_on[1];
		}
		else
		{
			pr_chemistry_on = print_chemistry_on[2];
		}
		phast_iphreeqc_worker->SetOutputFileOn(false);
		phast_iphreeqc_worker->SetOutputStringOn(pr_chemistry_on);
		phast_iphreeqc_worker->SetSelectedOutputFileOn(false);
		//if (!pr_chemistry_on)
		//{
		//	RunCellsThreadNoPrint(n);
		//}
		//else
		{
			phast_iphreeqc_worker->Get_cell_clock_times().clear();

			// selected output IPhreeqcPhast
			phast_iphreeqc_worker->CSelectedOutputMap.clear();	// Make a dummy run to fill in new CSelectedOutputMap
			if(this->selected_output_on)
			{
				phast_iphreeqc_worker->SetSelectedOutputStringOn(true);
				std::ostringstream input;
				input << "PRINT; -selected_output true\n";
				int next = phast_iphreeqc_worker->PhreeqcPtr->next_user_number(Keywords::KEY_SOLUTION);
				//input << "SOLUTION " << next << "; DELETE; -solution " << next << "\n";
				input << "SOLUTION " << next << ";";
				if (phast_iphreeqc_worker->PhreeqcPtr->llnl_temp.size() > 0)
				{
					input << "-temp " << phast_iphreeqc_worker->PhreeqcPtr->llnl_temp[0] << ";";
				}
				input << "DELETE; -solution " << next << "\n";
				if (phast_iphreeqc_worker->RunString(input.str().c_str()) < 0)
				{
					this->ErrorMessage(phast_iphreeqc_worker->GetErrorString());
					throw PhreeqcRMStop();
				}
				std::map< int, CSelectedOutput* >::iterator it = phast_iphreeqc_worker->SelectedOutputMap.begin();
				for ( ; it != phast_iphreeqc_worker->SelectedOutputMap.end(); it++)
				{
					int iso = it->first;
					// Add new item to CSelectedOutputMap
					CSelectedOutput cso;
					// Fill in columns
					phast_iphreeqc_worker->SetCurrentSelectedOutputUserNumber(iso);
					int columns = phast_iphreeqc_worker->GetSelectedOutputColumnCount();
					for (int i = 0; i < columns; i++)
					{
						VAR pvar, pvar1;
						VarInit(&pvar);
						VarInit(&pvar1);
						phast_iphreeqc_worker->GetSelectedOutputValue(0, i, &pvar);
						cso.PushBack(pvar.sVal, pvar1);
						VarClear(&pvar);
						VarClear(&pvar1);
					}
					phast_iphreeqc_worker->CSelectedOutputMap[iso] = cso;
				}
			}
			else
			{
				phast_iphreeqc_worker->SetSelectedOutputStringOn(false);
				std::ostringstream input;
				input << "PRINT; -selected_output false\n";
				if (phast_iphreeqc_worker->RunString(input.str().c_str()) < 0)
				{
					this->ErrorMessage(phast_iphreeqc_worker->GetErrorString());
					throw PhreeqcRMStop();
				}
			}

			std::vector<int> types;
			std::vector<long> longs;
			std::vector<double> doubles;
			std::string strings;

			// Do not write to files from phreeqc, run_cells writes files
			phast_iphreeqc_worker->SetLogFileOn(false);
			phast_iphreeqc_worker->SetSelectedOutputFileOn(false);
			phast_iphreeqc_worker->SetDumpFileOn(false);
			phast_iphreeqc_worker->SetDumpStringOn(false);
			phast_iphreeqc_worker->SetOutputFileOn(false);
			//phast_iphreeqc_worker->SetErrorFileOn(false);
#ifdef USE_MPI
			int start = this->start_cell[this->mpi_myself];
			int end = this->end_cell[this->mpi_myself];
#else
			int start = this->start_cell[n];
			int end = this->end_cell[n];
#endif
			// run the cells
			for (i = start; i <= end; i++)
			{							/* i is count_chem number */
				int local_chem_mask;
				bool calculation_success = true;
#ifdef USE_MPI
				phast_iphreeqc_worker->Get_cell_clock_times().push_back(- (double) MPI_Wtime());
				local_chem_mask = this->print_chem_mask_worker[i - start];
#elif defined(USE_OPENMP)
				j = backward_mapping[i][0];			/* j is nxyz number */
				phast_iphreeqc_worker->Get_cell_clock_times().push_back(- omp_get_wtime());
				local_chem_mask = this->print_chem_mask_root[j];
#else
				j = backward_mapping[i][0];			/* j is nxyz number */
				phast_iphreeqc_worker->Get_cell_clock_times().push_back(- (double) CLOCK());
				local_chem_mask = this->print_chem_mask_root[j];
#endif
				// Set local print flags
				bool pr_chem = pr_chemistry_on && (local_chem_mask != 0);

				// ignore small saturations
				bool active = true;
#ifdef USE_MPI
				double sat = saturation_worker[i - start];
#else
				double sat = saturation_root[j];
#endif
				if (sat <= 1e-6)
				{
					//this->saturation_root[j] = 0.0;
					active = false;
				}

				if (active)
				{

					// Set print flags
					phast_iphreeqc_worker->SetOutputStringOn(pr_chem);

					// do the calculation
					std::ostringstream input;
					input << "RUN_CELLS\n";
					input << "  -start_time " << (this->time - this->time_step) << "\n";
					input << "  -time_step  " << this->time_step << "\n";
					input << "  -cells      " << i << "\n";
					input << "END" << "\n";
					if (phast_iphreeqc_worker->RunString(input.str().c_str()) != 0)
					{
						if (this->GetErrorHandlerMode() < 3)
						{
							this->ErrorMessage(phast_iphreeqc_worker->GetErrorString());
							*phast_iphreeqc_worker->Get_out_stream() << phast_iphreeqc_worker->GetOutputString();
							throw PhreeqcRMStop();
						}
						else
						{
							calculation_success = false;
						}
					}
				}
				if (active && calculation_success)
				{
					// Write output file
					if (pr_chem)
					{
						std::ostringstream line_buff;
						line_buff << "Time:                   " << (this->time) * (this->time_conversion) << "\n";
						line_buff << "Chemistry cell:         " << i << "\n";
						line_buff << "Grid cell(s) (0-based): ";
						for (size_t ib = 0; ib < this->backward_mapping[i].size(); ib++)
						{
							line_buff << backward_mapping[i][ib] << " ";
						}
						line_buff << "\n";
						*phast_iphreeqc_worker->Get_out_stream() << line_buff.str();
						*phast_iphreeqc_worker->Get_out_stream() << phast_iphreeqc_worker->GetOutputString();
					}

					// Save selected output data
					if (this->selected_output_on)
					{
						// Add selected output values to IPhreeqcPhast CSelectedOutputMap's
						std::map< int, CSelectedOutput* >::iterator it = phast_iphreeqc_worker->SelectedOutputMap.begin();
						for ( ; it != phast_iphreeqc_worker->SelectedOutputMap.end(); it++)
						{
							int n_user = it->first;
							std::map< int, CSelectedOutput >::iterator ipp_it = phast_iphreeqc_worker->CSelectedOutputMap.find(n_user);
							assert(it->second->GetRowCount() == 2);
							if (ipp_it == phast_iphreeqc_worker->CSelectedOutputMap.end())
							{
								std::cerr << "Did not find item in CSelectedOutputMap" << std::endl;
								throw PhreeqcRMStop();
							}
							types.clear();
							longs.clear();
							doubles.clear();
							strings.clear();
							it->second->Serialize(0, types, longs, doubles, strings);
							ipp_it->second.DeSerialize(types, longs, doubles, strings);
						}
					}
				} // end active and calculation_success
				else
				{
					if (pr_chem)
					{
						std::ostringstream line_buff;
						line_buff << "Time:                   " << (this->time) * (this->time_conversion) << "\n";
						line_buff << "Chemistry cell:         " << i << "\n";
						line_buff << "Grid cell(s) (0-based): ";
						for (size_t ib = 0; ib < this->backward_mapping[i].size(); ib++)
						{
							line_buff << this->backward_mapping[i][ib] << " ";
						}
						if (calculation_success)
						{
							line_buff << "\nCell is dry.\n";
						}
						else
						{
							line_buff << "\nCalculation failure.\n";
						}
						*phast_iphreeqc_worker->Get_out_stream() << line_buff.str();
					}
					// Get selected output
					if (this->selected_output_on)
					{
						// Add selected output values to IPhreeqcPhast CSelectedOutputMap
						std::map< int, CSelectedOutput* >::iterator it = phast_iphreeqc_worker->SelectedOutputMap.begin();
						for ( ; it != phast_iphreeqc_worker->SelectedOutputMap.end(); it++)
						{
							int iso = it->first;
							std::map< int, CSelectedOutput >::iterator ipp_it = phast_iphreeqc_worker->CSelectedOutputMap.find(iso);
							ipp_it->second.EndRow();
						}
					}
				}
#ifdef USE_MPI
				phast_iphreeqc_worker->Get_cell_clock_times().back() += (double) MPI_Wtime();
#elif defined(USE_OPENMP)
				phast_iphreeqc_worker->Get_cell_clock_times().back() += omp_get_wtime();
#else
				phast_iphreeqc_worker->Get_cell_clock_times().back() += (double) CLOCK();
#endif
			} // end one cell

			// Copy selected output back to worker for Kinniburgh to process strings
			std::map< int, CSelectedOutput* >::iterator sit = phast_iphreeqc_worker->SelectedOutputMap.begin();
			for (; sit != phast_iphreeqc_worker->SelectedOutputMap.end(); ++sit)
			{
				delete (*sit).second;
			}
			phast_iphreeqc_worker->SelectedOutputMap.clear();

			std::map< int, CSelectedOutput >::iterator ipp_it = phast_iphreeqc_worker->CSelectedOutputMap.begin();
			for (; ipp_it != phast_iphreeqc_worker->CSelectedOutputMap.end(); ipp_it++)
			{
				CSelectedOutput* temp_ip_map = new CSelectedOutput(ipp_it->second);
				phast_iphreeqc_worker->SelectedOutputMap[ipp_it->first] = temp_ip_map;
			}
		}
		double t_elapsed = (double) CLOCK() - t0;
		phast_iphreeqc_worker->Set_thread_clock_time((double) t_elapsed);
	}
	catch (PhreeqcRMStop)
	{
		return IRM_FAIL;
	}
	catch (...)
	{
		std::ostringstream e_stream;
		e_stream << "Run cells failed in worker " << n << "from an unhandled exception.\n";
		this->ErrorMessage(e_stream.str());
		return IRM_FAIL;
	}
	return IRM_OK;
}
/* ---------------------------------------------------------------------- */
IRM_RESULT
PhreeqcRM::RunFile(bool workers, bool initial_phreeqc, bool utility, const std::string & chemistry_name)
/* ---------------------------------------------------------------------- */
{
	/*
	*  Run PHREEQC to obtain PHAST reactants
	*/
	this->phreeqcrm_error_string.clear();
#ifdef USE_MPI
	if (this->mpi_myself == 0)
	{
		int method = METHOD_RUNFILE;
		MPI_Bcast(&method, 1, MPI_INT, 0, phreeqcrm_comm);
	}
#endif
	this->error_count = 0;
	std::vector<int> flags;
	flags.resize(4);
	this->SetChemistryFileName(chemistry_name.c_str());
	if (mpi_myself == 0)
	{
		flags[0] = workers;
		flags[1] = initial_phreeqc;
		flags[2] = utility;
		flags[3] = this->error_count;
	}
#ifdef USE_MPI
	MPI_Bcast((void *) &flags.front(), 4, MPI_INT, 0, phreeqcrm_comm);
#endif

	// Quit on error
	if (flags[3] > 0)
	{
		return IRM_FAIL;
	}
	std::vector<bool> run;
	run.resize(this->nthreads + 2, false);
	std::vector < int >  r_vector;
	r_vector.resize(this->nthreads + 2, 0);

	// Set flag for each IPhreeqc instance
	if (flags[0] != 0)
	{
		for (int i = 0; i < this->nthreads; i++)
		{
			run[i] = true;
		}
	}
	if (flags[1] != 0)
	{
		run[this->nthreads] = true;
	}
	if (flags[2] != 0)
	{
		run[this->nthreads + 1] = true;
	}

#ifdef USE_OPENMP
	omp_set_num_threads(this->nthreads);
	#pragma omp parallel
	#pragma omp for
#endif
	for (int n = 0; n < this->nthreads + 2; n++)
	{
		if (run[n])
		{
			r_vector[n] = RunFileThread(n);
		}
	}
	// Check errors
	IRM_RESULT return_value = IRM_OK;
	try
	{
		HandleErrorsInternal(r_vector);
	}
	catch (...)
	{
		return_value = IRM_FAIL;
	}
	return this->ReturnHandler(return_value, "PhreeqcRM::RunFile");
}
/* ---------------------------------------------------------------------- */
IRM_RESULT
PhreeqcRM::RunFileThread(int n)
/* ---------------------------------------------------------------------- */
{
	try
	{
		IPhreeqcPhast * iphreeqc_phast_worker = this->GetWorkers()[n];

		iphreeqc_phast_worker->SetOutputFileOn(false);
		//iphreeqc_phast_worker->SetErrorFileOn(false);
		iphreeqc_phast_worker->SetLogFileOn(false);
		iphreeqc_phast_worker->SetSelectedOutputStringOn(false);
		iphreeqc_phast_worker->SetSelectedOutputFileOn(false);

		// Set output string on
		if (n < this->nthreads)
		{
			iphreeqc_phast_worker->SetOutputStringOn(this->print_chemistry_on[0]);
		}
		else if (n == this->nthreads)
		{
			iphreeqc_phast_worker->SetOutputStringOn(this->print_chemistry_on[1]);
		}
		else
		{
			iphreeqc_phast_worker->SetOutputStringOn(this->print_chemistry_on[2]);
		}

		// Run chemistry file
		if (iphreeqc_phast_worker->RunFile(this->chemistry_file_name.c_str()) > 0)
		{
			throw PhreeqcRMStop();
		}

		// Create a StorageBin with initial PHREEQC for boundary conditions
		if (iphreeqc_phast_worker->GetOutputStringOn())
		{
			this->OutputMessage(iphreeqc_phast_worker->GetOutputString());
		}
	}
	catch (PhreeqcRMStop)
	{
		return IRM_FAIL;
	}
	catch (...)
	{
		std::ostringstream e_stream;
		e_stream << "RunFile failed in worker " << n << "from an unhandled exception.\n";
		this->ErrorMessage(e_stream.str());
		return IRM_FAIL;
	}
	return IRM_OK;
}
/* ---------------------------------------------------------------------- */
IRM_RESULT
PhreeqcRM::RunString(bool workers, bool initial_phreeqc, bool utility, const std::string & input_string)
/* ---------------------------------------------------------------------- */
{
	/*
	*  Run PHREEQC to obtain PHAST reactants
	*/
	this->phreeqcrm_error_string.clear();
#ifdef USE_MPI
	if (this->mpi_myself == 0)
	{
		int method = METHOD_RUNSTRING;
		MPI_Bcast(&method, 1, MPI_INT, 0, phreeqcrm_comm);
	}
#endif
	this->error_count = 0;
	std::string input = input_string;
	std::vector<int> flags;
	flags.resize(5);
	if (mpi_myself == 0)
	{
		flags[0] = workers;
		flags[1] = initial_phreeqc;
		flags[2] = utility;
		flags[3] = (int) input.size();
		flags[4] = this->error_count;
	}
#ifdef USE_MPI
	MPI_Bcast((void *) &flags.front(), 5, MPI_INT, 0, phreeqcrm_comm);
	input.resize(flags[3]);
	MPI_Bcast((void *) input.c_str(), flags[3], MPI_CHAR, 0, phreeqcrm_comm);
#endif

	// Quit on error
	if (flags[4] > 0)
	{
		return IRM_FAIL;
	}

	// Set flag for each IPhreeqc instance
	std::vector<bool> run;
	run.resize(this->nthreads + 2, false);
	std::vector < int >  r_vector;
	r_vector.resize(this->nthreads + 2, 0);
	if (flags[0] != 0)
	{
		for (int i = 0; i < this->nthreads; i++)
		{
			run[i] = true;
		}
	}
	if (flags[1] != 0)
	{
		run[this->nthreads] = true;
	}
	if (flags[2] != 0)
	{
		run[this->nthreads + 1] = true;
	}
#ifdef USE_OPENMP
	omp_set_num_threads(this->nthreads);
	#pragma omp parallel
	#pragma omp for
#endif
	for (int n = 0; n < this->nthreads + 2; n++)
	{
		if (run[n])
		{
			r_vector[n] = RunStringThread(n, input);
		}
	}

	// Check errors
	IRM_RESULT return_value = IRM_OK;
	try
	{
		HandleErrorsInternal(r_vector);
	}
	catch (...)
	{
		return_value = IRM_FAIL;
	}
	return this->ReturnHandler(return_value, "PhreeqcRM::RunString");
}

/* ---------------------------------------------------------------------- */
IRM_RESULT
PhreeqcRM::RunStringThread(int n, std::string & input)
/* ---------------------------------------------------------------------- */
{
	try
	{
		IPhreeqcPhast * iphreeqc_phast_worker = this->GetWorkers()[n];

		iphreeqc_phast_worker->SetOutputFileOn(false);
		//iphreeqc_phast_worker->SetErrorFileOn(false);
		iphreeqc_phast_worker->SetLogFileOn(false);
		iphreeqc_phast_worker->SetSelectedOutputStringOn(false);
		iphreeqc_phast_worker->SetSelectedOutputFileOn(false);

		// Set output string on
		if (n < this->nthreads)
		{
			iphreeqc_phast_worker->SetOutputStringOn(this->print_chemistry_on[0]);
		}
		else if (n == this->nthreads)
		{
			iphreeqc_phast_worker->SetOutputStringOn(this->print_chemistry_on[1]);
		}
		else
		{
			iphreeqc_phast_worker->SetOutputStringOn(this->print_chemistry_on[2]);
		}
		// Run chemistry file
		if (iphreeqc_phast_worker->RunString(input.c_str()) > 0)
		{
			this->ErrorMessage(iphreeqc_phast_worker->GetErrorString());
			throw PhreeqcRMStop();
		}

		if (iphreeqc_phast_worker->GetOutputStringOn())
		{
			this->OutputMessage(iphreeqc_phast_worker->GetOutputString());
		}
	}
	catch (PhreeqcRMStop)
	{
		return IRM_FAIL;
	}
	catch (...)
	{
		std::ostringstream e_stream;
		e_stream << "RunString failed in worker " << n << "from an unhandled exception.\n";
		this->ErrorMessage(e_stream.str());
		return IRM_FAIL;
	}
	return IRM_OK;
}
/* ---------------------------------------------------------------------- */
void
PhreeqcRM::Scale_solids(int n, int iphrq, LDBLE frac)
/* ---------------------------------------------------------------------- */
{
	int n_user;

	/*
	 * repartition solids for partially saturated cells
	 */

	n_user = iphrq;
	cxxMix cxxmix;
	cxxmix.Add(n_user, frac);
	/*
	 *   Scale compositions
	 */
	cxxStorageBin sz_bin;
	IPhreeqcPhast *phast_iphreeqc_worker = this->workers[n];
	phast_iphreeqc_worker->Put_cell_in_storage_bin(sz_bin, n_user);
	if (sz_bin.Get_Exchange(n_user) != NULL)
	{
		cxxExchange cxxentity(sz_bin.Get_Exchangers(), cxxmix, n_user);
		sz_bin.Set_Exchange(n_user, &cxxentity);
	}
	if (sz_bin.Get_PPassemblage(n_user) != NULL)
	{
		cxxPPassemblage cxxentity(sz_bin.Get_PPassemblages(), cxxmix, n_user);
		sz_bin.Set_PPassemblage(n_user, &cxxentity);
	}
	if (sz_bin.Get_GasPhase(n_user) != NULL)
	{
		cxxGasPhase cxxentity(sz_bin.Get_GasPhases(), cxxmix, n_user);
		sz_bin.Set_GasPhase(n_user, &cxxentity);
	}
	if (sz_bin.Get_SSassemblage(n_user) != NULL)
	{
		cxxSSassemblage cxxentity(sz_bin.Get_SSassemblages(), cxxmix, n_user);
		sz_bin.Set_SSassemblage(n_user, &cxxentity);
	}
	if (sz_bin.Get_Kinetics(n_user) != NULL)
	{
		cxxKinetics cxxentity(sz_bin.Get_Kinetics(), cxxmix, n_user);
		sz_bin.Set_Kinetics(n_user, &cxxentity);
	}
	if (sz_bin.Get_Surface(n_user) != NULL)
	{
		cxxSurface cxxentity(sz_bin.Get_Surfaces(), cxxmix, n_user);
		sz_bin.Set_Surface(n_user, &cxxentity);
	}
	phast_iphreeqc_worker->Get_cell_from_storage_bin(sz_bin, n_user);
	return;
}
/* ---------------------------------------------------------------------- */
void
PhreeqcRM::ScatterNchem(int *i_array)
/* ---------------------------------------------------------------------- */
{
#ifdef USE_MPI
	int * send_buf = NULL;
	int * send_counts = NULL;
	int * send_displs = NULL;
	int * recv_buf = NULL;
	int *recv_dummy = NULL;
	int recv_count;
	recv_count = (end_cell[this->mpi_myself] - start_cell[this->mpi_myself] + 1) * 7;
	if (this->mpi_myself == 0)
	{
		send_buf = &i_array[0];
		send_counts = new int[this->mpi_tasks];
		send_displs = new int[this->mpi_tasks];
		for (int j = 0; j < this->mpi_tasks; j++)
		{
			send_counts[j] = end_cell[j] - start_cell[j] + 1;
			send_displs[j] = start_cell[j];
		}
		recv_dummy = new int[send_counts[0]];
		recv_buf = &recv_dummy[0];
	}
	else
	{
		recv_buf = &i_array[0];
	}

	MPI_Scatterv(send_buf, send_counts, send_displs, MPI_INT, recv_buf, recv_count, MPI_INT, 0, phreeqcrm_comm);
	if (this->mpi_myself == 0)
	{
		delete [] recv_dummy;
	}
	delete [] send_counts;
	delete [] send_displs;
#endif
}
/* ---------------------------------------------------------------------- */
void
PhreeqcRM::ScatterNchem(double *d_array)
/* ---------------------------------------------------------------------- */
{
#ifdef USE_MPI
	double * send_buf = NULL;
	int * send_counts = NULL;
	int * send_displs = NULL;
	double * recv_buf = NULL;
	double *recv_dummy = NULL;
	int recv_count;
	recv_count = end_cell[this->mpi_myself] - start_cell[this->mpi_myself] + 1;
	if (this->mpi_myself == 0)
	{
		send_buf = &d_array[0];
		send_counts = new int[this->mpi_tasks];
		send_displs = new int[this->mpi_tasks];
		for (int j = 0; j < this->mpi_tasks; j++)
		{
			send_counts[j] = end_cell[j] - start_cell[j] + 1;
			send_displs[j] = start_cell[j];
		}
		recv_dummy = new double[send_counts[0]];
		recv_buf = &recv_dummy[0];
	}
	else
	{
		recv_buf = &d_array[0];
	}

	MPI_Scatterv(send_buf, send_counts, send_displs, MPI_DOUBLE, recv_buf, recv_count, MPI_DOUBLE, 0, this->phreeqcrm_comm);
	if (this->mpi_myself == 0)
	{
		delete [] recv_dummy;
	}
	delete [] send_counts;
	delete [] send_displs;
#endif
}

/* ---------------------------------------------------------------------- */
void
PhreeqcRM::ScatterNchem(std::vector<double> &source, std::vector<double> &destination)
/* ---------------------------------------------------------------------- */
{
	// source is nxyz on root
	// destination is nchem pieces on workers
#ifdef USE_MPI
	std::vector<double> d_array;
	d_array.resize(this->count_chemistry, INACTIVE_CELL_VALUE);
	if (mpi_myself == 0)
	{
		for (int j = 0; j < this->count_chemistry; j++)
		{
			int ixyz = this->backward_mapping[j][0];
			d_array[j] = source[ixyz];
		}
	}

	double * send_buf = NULL;
	int * send_counts = NULL;
	int * send_displs = NULL;
	double * recv_buf = NULL;
	int recv_count;
	recv_count = end_cell[this->mpi_myself] - start_cell[this->mpi_myself] + 1;
	destination.resize(recv_count);
	recv_buf = &(destination[0]);

	if (mpi_myself == 0)
	{
		send_buf = &d_array[0];
		send_counts = new int[this->mpi_tasks];
		send_displs = new int[this->mpi_tasks];
		for (int j = 0; j < this->mpi_tasks; j++)
		{
			send_counts[j] = end_cell[j] - start_cell[j] + 1;
			send_displs[j] = start_cell[j];
		}
	}

	MPI_Scatterv(send_buf, send_counts, send_displs, MPI_DOUBLE, recv_buf, recv_count, MPI_DOUBLE, 0, this->phreeqcrm_comm);

	delete [] send_counts;
	delete [] send_displs;
#endif
}
/* ---------------------------------------------------------------------- */
void
PhreeqcRM::ScatterNchem(std::vector<int> &source, std::vector<int> &destination)
/* ---------------------------------------------------------------------- */
{
	// source is nxyz on root
	// destination is nchem pieces on workers
#ifdef USE_MPI
	std::vector<int> i_array;
	i_array.resize(this->count_chemistry);
	if (mpi_myself == 0)
	{
		for (int j = 0; j < this->count_chemistry; j++)
		{
			int ixyz = this->backward_mapping[j][0];
			i_array[j] = source[ixyz];
		}
	}

	int * send_buf = NULL;
	int * send_counts = NULL;
	int * send_displs = NULL;
	int * recv_buf = NULL;
	int recv_count;
	recv_count = end_cell[this->mpi_myself] - start_cell[this->mpi_myself] + 1;
	destination.resize(recv_count);
	recv_buf = &(destination[0]);

	if (mpi_myself == 0)
	{
		send_buf = &i_array[0];
		send_counts = new int[this->mpi_tasks];
		send_displs = new int[this->mpi_tasks];
		for (int j = 0; j < this->mpi_tasks; j++)
		{
			send_counts[j] = end_cell[j] - start_cell[j] + 1;
			send_displs[j] = start_cell[j];
		}
	}

	MPI_Scatterv(send_buf, send_counts, send_displs, MPI_INT, recv_buf, recv_count, MPI_INT, 0, this->phreeqcrm_comm);

	delete [] send_counts;
	delete [] send_displs;
#endif
}
/* ---------------------------------------------------------------------- */
void
PhreeqcRM::ScreenMessage(const std::string &str)
/* ---------------------------------------------------------------------- */
{
	this->phreeqcrm_io->screen_msg(str.c_str());
}
/* ---------------------------------------------------------------------- */
IRM_RESULT
PhreeqcRM::SetChemistryFileName(const char * cn)
/* ---------------------------------------------------------------------- */
{
	this->phreeqcrm_error_string.clear();
	IRM_RESULT return_value = IRM_OK;
	int l = 0;
	if (this->mpi_myself == 0)
	{
		this->chemistry_file_name = Char2TrimString(cn);
		l = (int) this->chemistry_file_name.size();
	}
#ifdef USE_MPI
	MPI_Bcast(&l, 1, MPI_INT, 0, phreeqcrm_comm);
	if (l > 0)
	{
		this->chemistry_file_name.resize(l);
		MPI_Bcast((void *) this->chemistry_file_name.c_str(), l, MPI_CHAR, 0, phreeqcrm_comm);
	}
#endif
	if (l == 0)
	{
		return_value = IRM_INVALIDARG;
	}
	return this->ReturnHandler(return_value, "PhreeqcRM::SetChemistryFileName");
}
/* ---------------------------------------------------------------------- */
IRM_RESULT
PhreeqcRM::SetComponentH2O(bool tf)
/* ---------------------------------------------------------------------- */
{
	this->phreeqcrm_error_string.clear();
#ifdef USE_MPI
	if (this->mpi_myself == 0)
	{
		int method = METHOD_SETCOMPONENTH2O;
		MPI_Bcast(&method, 1, MPI_INT, 0, phreeqcrm_comm);
	}
#endif
	IRM_RESULT return_value = IRM_OK;
	if (mpi_myself == 0)
	{
		this->component_h2o  = tf;
	}
#ifdef USE_MPI
	int temp_tf = this->component_h2o ? 1 : 0;
	MPI_Bcast(&temp_tf,  1, MPI_INT, 0, phreeqcrm_comm);
	this->component_h2o = (temp_tf == 0) ? false : true;
	//MPI_Bcast(&this->component_h2o,  1, MPI_LOGICAL, 0, phreeqcrm_comm);
#endif
	return this->ReturnHandler(return_value, "PhreeqcRM::SetComponentH2O");
}
/* ---------------------------------------------------------------------- */
IRM_RESULT
PhreeqcRM::SetConcentrations(const std::vector<double> &t)
/* ---------------------------------------------------------------------- */
{
	this->phreeqcrm_error_string.clear();
	IRM_RESULT return_value = IRM_OK;
	std::vector<double> c_chem, c_chem_root;
	c_chem.resize(this->count_chemistry * (int) this->components.size(), INACTIVE_CELL_VALUE);

	if (this->mpi_myself == 0)
	{
		c_chem_root.resize(this->count_chemistry * (int) this->components.size(), INACTIVE_CELL_VALUE);
		for (int i = 0; i < this->count_chemistry; i++)
		{
			int j = this->backward_mapping[i][0];
			for (int k = 0; k < (int) this->components.size(); k++)
			{
				c_chem_root[i * (int) this->components.size() + k] = t[k*this->nxyz + j];
			}
		}
	}
#ifdef USE_MPI
	if (this->mpi_myself == 0)
	{
		int method = METHOD_SETCONCENTRATIONS;
		MPI_Bcast(&method, 1, MPI_INT, 0, phreeqcrm_comm);
	}

	double * send_buf = NULL;
	int * send_counts = NULL;
	int * send_displs = NULL;
	double * recv_buf = NULL;
	int recv_count;
	recv_count = (end_cell[this->mpi_myself] - start_cell[this->mpi_myself] + 1) * (int) this->components.size();
	if (this->mpi_myself == 0)
	{
		send_buf = &c_chem_root[0];
		send_counts = new int[this->mpi_tasks];
		send_displs = new int[this->mpi_tasks];
		for (int j = 0; j < this->mpi_tasks; j++)
		{
			send_counts[j] = (end_cell[j] - start_cell[j] + 1) * (int) this->components.size();
			send_displs[j] = start_cell[j] * (int) this->components.size();
		}
		recv_buf = &c_chem[0];
	}
	else
	{
		recv_buf = &c_chem[start_cell[this->mpi_myself] * (int) this->components.size()];
	}

	MPI_Scatterv(send_buf, send_counts, send_displs, MPI_DOUBLE, recv_buf, recv_count, MPI_DOUBLE, 0, phreeqcrm_comm);
	delete [] send_counts;
	delete [] send_displs;
#endif

#ifdef USE_OPENMP
	omp_set_num_threads(this->nthreads);
#pragma omp parallel
#pragma omp for
#endif
	for (int n = 0; n < nthreads; n++)
	{
#ifdef USE_MPI
		this->Concentrations2Solutions(n, c_chem);
#else
		this->Concentrations2Solutions(n, c_chem_root);
#endif
	}
	this->UpdateBMI(RMVARS::Concentrations);
	return this->ReturnHandler(return_value, "PhreeqcRM::SetConcentrations");
}
#ifdef ORIG
/* ---------------------------------------------------------------------- */
IRM_RESULT
PhreeqcRM::SetGasCompMoles(const std::vector<double>& t)
/* ---------------------------------------------------------------------- */
{
	this->phreeqcrm_error_string.clear();
	IRM_RESULT return_value = IRM_OK;
	std::vector<double> gas_moles_root;
	if (mpi_myself == 0)
	{
		assert(t.size() >= this->nxyz * this->GasComponentsList.size());
	}
	if (this->mpi_myself == 0)
	{
		gas_moles_root.resize((size_t)this->count_chemistry * this->GasComponentsList.size(), -1.0);
		for (size_t i = 0; i < (size_t)this->count_chemistry; i++)
		{
			int j = this->backward_mapping[i][0];
			for (size_t k = 0; k < this->GasComponentsList.size(); k++)
			{
				gas_moles_root[i * this->GasComponentsList.size() + k] = t[k * this->nxyz + j];
			}
		}
	}

#ifdef USE_MPI
	if (this->mpi_myself == 0)
	{
		int method = METHOD_SETGASCOMPMOLES;
		MPI_Bcast(&method, 1, MPI_INT, 0, phreeqcrm_comm);
	}

	double* send_buf = NULL;
	int* send_counts = NULL;
	int* send_displs = NULL;
	double* recv_buf = NULL;
	int recv_count;

	std::vector<double> gas_moles;
	gas_moles.resize((size_t)this->count_chemistry * (int)this->GasComponentsList.size(), -1.0);

	recv_count = (end_cell[this->mpi_myself] - start_cell[this->mpi_myself] + 1) * (int)this->GasComponentsList.size();

	if (this->mpi_myself == 0)
	{
		send_buf = &gas_moles_root[0];
		send_counts = new int[this->mpi_tasks];
		send_displs = new int[this->mpi_tasks];
		for (int j = 0; j < this->mpi_tasks; j++)
		{
			send_counts[j] = (end_cell[j] - start_cell[j] + 1) * (int)this->GasComponentsList.size();
			send_displs[j] = start_cell[j] * (int)this->GasComponentsList.size();
		}
		recv_buf = &gas_moles[0];
	}
	else
	{
		recv_buf = &gas_moles[start_cell[this->mpi_myself] * this->GasComponentsList.size()];
	}
	MPI_Scatterv(send_buf, send_counts, send_displs, MPI_DOUBLE, recv_buf, recv_count, MPI_DOUBLE, 0, phreeqcrm_comm);

	delete[] send_counts;
	delete[] send_displs;

	int n = mpi_myself;
	for (size_t j = (size_t)this->start_cell[n]; j <= (size_t)this->end_cell[n]; j++)
	{
		cxxGasPhase temp_gas;
		for (size_t k = 0; k < this->GasComponentsList.size(); k++)
		{
			double moles = gas_moles[j * (size_t)this->GasComponentsList.size() + k];
			if (moles >= 0.0)
			{
				cxxGasComp temp_comp;
				temp_comp.Set_phase_name(GasComponentsList[k]);
				temp_comp.Set_moles(moles);
				temp_gas.Get_gas_comps().push_back(temp_comp);
			}
		}

		cxxGasPhase* gas_ptr = this->GetWorkers()[0] ->Get_gas_phase((int)j);
		if (temp_gas.Get_gas_comps().size() > 0)
		{
			if (gas_ptr != NULL)
			{
				gas_ptr->Set_gas_comps(temp_gas.Get_gas_comps());
			}
			else
			{
				this->GetWorkers()[n]->Get_PhreeqcPtr()->Rxn_gas_phase_map[(int)j] = temp_gas;
			}
		}
		else
		{
			if (gas_ptr != NULL)
			{
				this->GetWorkers()[n]->Get_PhreeqcPtr()->Rxn_gas_phase_map.erase((int)j);
			}
		}
	}
#else
#ifdef USE_OPENMP
	omp_set_num_threads(this->nthreads);
#pragma omp parallel
#pragma omp for
#endif
	for (int n = 0; n < nthreads; n++)
	{
		for (size_t j = (size_t)this->start_cell[n]; j <= (size_t)this->end_cell[n]; j++)
		{
			cxxGasPhase* gas_ptr = this->GetWorkers()[(int)n]->Get_gas_phase((int)j);
			for (size_t k = 0; k < this->GasComponentsList.size(); k++)
			{
				double moles = gas_moles_root[j * this->GasComponentsList.size() + k];
				if (moles >= 0.0)
				{
					gas_ptr->Set_component_moles(this->GasComponentsList[k], moles);
				}
				else
				{
					gas_ptr->Delete_component(this->GasComponentsList[k]);
				}
			}
			if (gas_ptr->Get_gas_comps().size() == 0)
			{
				this->GetWorkers()[n]->Get_PhreeqcPtr()->Rxn_gas_phase_map.erase((int)j);
			}
		}
	}
#endif
	return this->ReturnHandler(return_value, "PhreeqcRM::SetGasCompMoles");
}
#endif

/* ---------------------------------------------------------------------- */
IRM_RESULT
PhreeqcRM::SetGasCompMoles(const std::vector<double>& m_in)
/* ---------------------------------------------------------------------- */
{
	this->phreeqcrm_error_string.clear();
	IRM_RESULT return_value = IRM_OK;
	std::vector<double> send_gas_moles;
	if (mpi_myself == 0)
	{
		assert(m_in.size() >= this->nxyz * this->GasComponentsList.size());
	}
	if (this->mpi_myself == 0)
	{
		send_gas_moles.resize((size_t)this->count_chemistry * this->GasComponentsList.size(), -1.0);
		for (size_t i = 0; i < (size_t)this->count_chemistry; i++)
		{
			int j = this->backward_mapping[i][0];
			for (size_t k = 0; k < this->GasComponentsList.size(); k++)
			{
				send_gas_moles[i * this->GasComponentsList.size() + k] = m_in[k * this->nxyz + j];
			}
		}
	}
#ifdef USE_MPI
	if (this->mpi_myself == 0)
	{
		int method = METHOD_SETGASCOMPMOLES;
		MPI_Bcast(&method, 1, MPI_INT, 0, phreeqcrm_comm);
	}
	double* send_buf = NULL;
	int* send_counts = NULL;
	int* send_displs = NULL;
	double* recv_buf = NULL;
	int recv_count;

	std::vector<double> recv_gas_moles;
	recv_gas_moles.resize((size_t)this->count_chemistry * (int)this->GasComponentsList.size(), -1.0);

	recv_count = (end_cell[this->mpi_myself] - start_cell[this->mpi_myself] + 1) * (int)this->GasComponentsList.size();

	if (this->mpi_myself == 0)
	{
		send_buf = &send_gas_moles[0];
		send_counts = new int[this->mpi_tasks];
		send_displs = new int[this->mpi_tasks];
		for (int j = 0; j < this->mpi_tasks; j++)
		{
			send_counts[j] = (end_cell[j] - start_cell[j] + 1) * (int)this->GasComponentsList.size();
			send_displs[j] = start_cell[j] * (int)this->GasComponentsList.size();
		}
		recv_buf = &recv_gas_moles[0];
	}
	else
	{
		recv_buf = &recv_gas_moles[start_cell[this->mpi_myself] * this->GasComponentsList.size()];
	}
	MPI_Scatterv(send_buf, send_counts, send_displs, MPI_DOUBLE, recv_buf, recv_count, MPI_DOUBLE, 0, phreeqcrm_comm);

	delete[] send_counts;
	delete[] send_displs;

	int n = mpi_myself;
	for (size_t j = (size_t)this->start_cell[n]; j <= (size_t)this->end_cell[n]; j++)
	{
		cxxGasPhase* gas_ptr = this->GetWorkers()[0]->Get_gas_phase((int)j);
		if (gas_ptr != NULL) // Gas phase exists
		{
			for (size_t k = 0; k < this->GasComponentsList.size(); k++)
			{
				double moles = recv_gas_moles[j * this->GasComponentsList.size() + k];
				if (moles >= 0.0)
					gas_ptr->Set_component_moles(this->GasComponentsList[k], moles);
				else
					gas_ptr->Delete_component(this->GasComponentsList[k]);
			}
			if (gas_ptr->Get_gas_comps().size() == 0)
				this->GetWorkers()[n]->Get_PhreeqcPtr()->Rxn_gas_phase_map.erase((int)j);
		}
		else  // no gas phase
		{
			cxxGasPhase temp_gas;
			for (size_t k = 0; k < this->GasComponentsList.size(); k++)
			{
				double moles = recv_gas_moles[j * (size_t)this->GasComponentsList.size() + k];
				if (moles >= 0.0)
				{
					cxxGasComp temp_comp;
					temp_comp.Set_phase_name(GasComponentsList[k]);
					temp_comp.Set_moles(moles);
					temp_gas.Get_gas_comps().push_back(temp_comp);
				}
			}
			if (temp_gas.Get_gas_comps().size() > 0)
			{
				this->GetWorkers()[n]->Get_PhreeqcPtr()->Rxn_gas_phase_map[(int)j] = temp_gas;
			}
		}
	}
#else
#ifdef USE_OPENMP
	omp_set_num_threads(this->nthreads);
#pragma omp parallel
#pragma omp for
#endif
	for (int n = 0; n < nthreads; n++)
	{
		for (size_t j = (size_t)this->start_cell[n]; j <= (size_t)this->end_cell[n]; j++)
		{
			cxxGasPhase* gas_ptr = this->GetWorkers()[(int)n]->Get_gas_phase((int)j);
			if (gas_ptr != NULL) // gas phase exists
			{
				for (size_t k = 0; k < this->GasComponentsList.size(); k++)
				{
					double moles = send_gas_moles[j * this->GasComponentsList.size() + k];
					if (moles >= 0.0)
						gas_ptr->Set_component_moles(this->GasComponentsList[k], moles);
					else
						gas_ptr->Delete_component(this->GasComponentsList[k]);
				}
				if (gas_ptr->Get_gas_comps().size() == 0)
					this->GetWorkers()[n]->Get_PhreeqcPtr()->Rxn_gas_phase_map.erase((int)j);
			}
			else  // no gas phase
			{
				cxxGasPhase temp_gas;
				for (size_t k = 0; k < this->GasComponentsList.size(); k++)
				{
					double moles = send_gas_moles[j * (size_t)this->GasComponentsList.size() + k];
					if (moles >= 0.0)
					{
						cxxGasComp temp_comp;
						temp_comp.Set_phase_name(GasComponentsList[k]);
						temp_comp.Set_moles(moles);
						temp_gas.Get_gas_comps().push_back(temp_comp);
					}
				}

				if (temp_gas.Get_gas_comps().size() > 0)
				{
					this->GetWorkers()[n]->Get_PhreeqcPtr()->Rxn_gas_phase_map[(int)j] = temp_gas;
				}
			}
		}
	}
#endif
	return this->ReturnHandler(return_value, "PhreeqcRM::SetGasCompMoles");
}

/* ---------------------------------------------------------------------- */
IRM_RESULT
PhreeqcRM::SetGasPhaseVolume(const std::vector<double>& v_in)
/* ---------------------------------------------------------------------- */
{
	this->phreeqcrm_error_string.clear();
	IRM_RESULT return_value = IRM_OK;
	std::vector<double> send_gas_volume;
	if (mpi_myself == 0)
	{
		assert(v_in.size() >= this->nxyz);
	}
	if (this->mpi_myself == 0)
	{
		send_gas_volume.resize((size_t)this->count_chemistry, -1.0);
		for (size_t i = 0; i < (size_t)this->count_chemistry; i++)
		{
			int j = this->backward_mapping[i][0];
			send_gas_volume[i] = v_in[j];
		}
	}

#ifdef USE_MPI
	if (this->mpi_myself == 0)
	{
		int method = METHOD_SETGASPHASEVOLUME;
		MPI_Bcast(&method, 1, MPI_INT, 0, phreeqcrm_comm);
	}

	double* send_buf = NULL;
	int* send_counts = NULL;
	int* send_displs = NULL;
	double* recv_buf = NULL;
	int recv_count;

	std::vector<double> recv_gas_volume;
	recv_gas_volume.resize((size_t)this->count_chemistry, -1.0);

	recv_count = (end_cell[this->mpi_myself] - start_cell[this->mpi_myself] + 1);

	if (this->mpi_myself == 0)
	{
		send_buf = &send_gas_volume[0];
		send_counts = new int[this->mpi_tasks];
		send_displs = new int[this->mpi_tasks];
		for (int j = 0; j < this->mpi_tasks; j++)
		{
			send_counts[j] = (end_cell[j] - start_cell[j] + 1);
			send_displs[j] = start_cell[j];
		}
		recv_buf = &recv_gas_volume[0];
	}
	else
	{
		recv_buf = &recv_gas_volume[start_cell[this->mpi_myself] ];
	}
	MPI_Scatterv(send_buf, send_counts, send_displs, MPI_DOUBLE, recv_buf, recv_count, MPI_DOUBLE, 0, phreeqcrm_comm);

	delete[] send_counts;
	delete[] send_displs;

	int n = mpi_myself;
	for (size_t j = (size_t)this->start_cell[n]; j <= (size_t)this->end_cell[n]; j++)
	{
		double volume = recv_gas_volume[j];
		if (volume < 0.0) continue;
		cxxGasPhase* gas_ptr = this->GetWorkers()[0]->Get_gas_phase((int)j);
		if (gas_ptr != NULL) // Gas phase exists
		{
			gas_ptr->Set_volume(volume);
			gas_ptr->Set_type(cxxGasPhase::GP_VOLUME);
		}
		else  // no gas phase
		{
			cxxGasPhase temp_gas;
			temp_gas.Set_type(cxxGasPhase::GP_VOLUME);
			temp_gas.Set_volume(volume);
			this->GetWorkers()[n]->Get_PhreeqcPtr()->Get_Rxn_gas_phase_map()[(int)j] = temp_gas;
		}
	}
#else
#ifdef USE_OPENMP
	omp_set_num_threads(this->nthreads);
#pragma omp parallel
#pragma omp for
#endif
	for (int n = 0; n < nthreads; n++)
	{
		for (size_t j = (size_t)this->start_cell[n]; j <= (size_t)this->end_cell[n]; j++)
		{
			if (send_gas_volume[j] < 0.0) continue;
			cxxGasPhase* gas_ptr = this->GetWorkers()[(int)n]->Get_gas_phase((int)j);
			if (gas_ptr != NULL) // gas phase exists
			{
				gas_ptr->Set_volume(send_gas_volume[j]);
				gas_ptr->Set_type(cxxGasPhase::GP_VOLUME);
			}
			else  // no gas phase
			{
				cxxGasPhase temp_gas;
				temp_gas.Set_type(cxxGasPhase::GP_VOLUME);
				temp_gas.Set_volume(send_gas_volume[j]);
				this->GetWorkers()[n]->Get_PhreeqcPtr()->Get_Rxn_gas_phase_map()[(int)j] = temp_gas;
			}
		}
	}
#endif
	return this->ReturnHandler(return_value, "PhreeqcRM::SetGasPhaseVolume");
}

/* ---------------------------------------------------------------------- */
IRM_RESULT
PhreeqcRM::SetCurrentSelectedOutputUserNumber(int i)
{
	this->phreeqcrm_error_string.clear();
	int return_value = IRM_INVALIDARG;
	if (i >= 0)
	{
		return_value = this->workers[0]->SetCurrentSelectedOutputUserNumber(i);
	}
	return this->ReturnHandler(PhreeqcRM::Int2IrmResult(return_value, false),"PhreeqcRM::SetCurrentSelectedOutputUserNumber");
}
/* ---------------------------------------------------------------------- */
IRM_RESULT
PhreeqcRM::SetDatabaseFileName(const char * db)
/* ---------------------------------------------------------------------- */
{
	IRM_RESULT return_value = IRM_OK;
	int l = 0;
	if (this->mpi_myself == 0)
	{
		this->database_file_name = Char2TrimString(db);
		l = (int) this->database_file_name.size();
	}
#ifdef USE_MPI
	MPI_Bcast(&l, 1, MPI_INT, 0, phreeqcrm_comm);
	if (l > 0)
	{
		this->database_file_name.resize(l);
		MPI_Bcast((void *) this->database_file_name.c_str(), l, MPI_CHAR, 0, phreeqcrm_comm);
	}
#endif
	if (l == 0)
	{
		return_value = IRM_INVALIDARG;
	}
	return this->ReturnHandler(return_value, "PhreeqcRM::SetDatabaseFileName");
}

/* ---------------------------------------------------------------------- */
IRM_RESULT
PhreeqcRM::SetDensity(const std::vector<double>& t)
/* ---------------------------------------------------------------------- */
{
	// For backward compatibility
	return SetDensityUser(t);
}
/* ---------------------------------------------------------------------- */
IRM_RESULT
PhreeqcRM::SetDensityUser(const std::vector<double> &t)
/* ---------------------------------------------------------------------- */
{
	this->phreeqcrm_error_string.clear();
	std::string methodName = "SetDensityUser";
	IRM_RESULT result_value = SetGeneric(t, this->density_root, density_worker, METHOD_SETDENSITYUSER, methodName);
	this->UpdateBMI(RMVARS::DensityCalculated);
	return this->ReturnHandler(result_value, "PhreeqcRM::" + methodName);
}

/* ---------------------------------------------------------------------- */
IRM_RESULT
PhreeqcRM::SetDumpFileName(const std::string & cn)
/* ---------------------------------------------------------------------- */
{
	this->phreeqcrm_error_string.clear();
	IRM_RESULT return_value = IRM_OK;
	int l = 0;
	if (this->mpi_myself == 0)
	{
		if (cn.size() > 0)
		{
			this->dump_file_name = cn;
			l = (int) this->dump_file_name.size();
		}
		else
		{
			this->dump_file_name = this->file_prefix;
			this->dump_file_name.append(".dmp");
		}
	}

	if (l == 0)
	{
		return_value = IRM_INVALIDARG;
	}
	return this->ReturnHandler(return_value, "PhreeqcRM::SetDumpFileName");
}
/* ---------------------------------------------------------------------- */
void
PhreeqcRM::SetEndCells(void)
/* ---------------------------------------------------------------------- */
{
#ifdef USE_MPI
	int ntasks = this->mpi_tasks;
#else
	int ntasks = this->nthreads;
#endif
	int n = this->count_chemistry / ntasks;
	int extra = this->count_chemistry - n*ntasks;
	std::vector<int> cells;
	for (int i = 0; i < extra; i++)
	{
		cells.push_back(n+1);
	}
	for (int i = extra; i < ntasks; i++)
	{
		cells.push_back(n);
	}
	int cell0 = 0;
	start_cell.clear();
	end_cell.clear();
	for (int i = 0; i < ntasks; i++)
	{
		this->start_cell.push_back(cell0);
		this->end_cell.push_back(cell0 + cells[i] - 1);
		cell0 = cell0 + cells[i];
	}
}
/* ---------------------------------------------------------------------- */
void
PhreeqcRM::SetEndCellsHeterogeneous(void)
/* ---------------------------------------------------------------------- */
{
#ifdef USE_MPI
	int ntasks = this->mpi_tasks;
#else
	int ntasks = this->nthreads;
#endif

	std::vector<double> standard_time, task_fraction;
	if (mpi_myself == 0)
	{
		double tasks_total = 0;
		for (size_t i = 0; i < (size_t) mpi_tasks; i++)
		{
			standard_time.push_back(this->standard_task_vector[i]);   // slower is bigger number, as in more time
			//standard_time.push_back(1.0);                           // homogeneous
			tasks_total += 1.0 / standard_time[i];
		}

		for (size_t i = 0; i < (size_t) mpi_tasks; i++)
		{
			task_fraction.push_back((1.0 / standard_time[i]) / tasks_total);
		}

		std::vector<int> cells;
		int sum = 0;
		for(int i = 0; i < this->mpi_tasks; i++)
		{
			cells.push_back((int) (task_fraction[i] * this->count_chemistry));
			sum += cells[i];
		}

		int extra = this->count_chemistry - sum;
		while (extra > 0)
		{
			for(int i = 0; i < this->mpi_tasks; i++)
			{
				cells[i]++;
				extra--;
				if (extra == 0) break;
			}
		}

		int cell0 = 0;
		start_cell.clear();
		end_cell.clear();
		for (int i = 0; i < ntasks; i++)
		{
			this->start_cell.push_back(cell0);
			this->end_cell.push_back(cell0 + cells[i] - 1);
			cell0 = cell0 + cells[i];
		}
	}
	else
	{
		start_cell.resize(this->mpi_tasks);
		end_cell.resize(this->mpi_tasks);
	}
#ifdef USE_MPI
	MPI_Bcast((void *) &start_cell.front(), mpi_tasks, MPI_INT, 0, phreeqcrm_comm);
	MPI_Bcast((void *) &end_cell.front(), mpi_tasks, MPI_INT, 0, phreeqcrm_comm);
#endif
}
/* ---------------------------------------------------------------------- */
IRM_RESULT
PhreeqcRM::SetErrorHandlerMode(int i)
/* ---------------------------------------------------------------------- */
{
	this->phreeqcrm_error_string.clear();
#ifdef USE_MPI
		if (this->mpi_myself == 0)
		{
			if (this->error_handler_mode == i) return IRM_OK;
			int method = METHOD_SETERRORHANDLERMODE;
			MPI_Bcast(&method, 1, MPI_INT, 0, phreeqcrm_comm);
		}
#endif
	IRM_RESULT return_value = IRM_OK;
	if (mpi_myself == 0)
	{
		this->error_handler_mode = 0;
		if (i < 0)
		{
			return_value = IRM_INVALIDARG;
		}
		else if (i < 3)
		{
			this->error_handler_mode = i;
		}
#ifdef USE_OPENMP
		else if (i == 3)
		{
			this->error_handler_mode = i;
		}
#endif
		else
		{
			return_value = IRM_INVALIDARG;
		}
	}
#ifdef USE_MPI
	MPI_Bcast(&this->error_handler_mode, 1, MPI_INT, 0, phreeqcrm_comm);
#endif
	return this->ReturnHandler(return_value, "PhreeqcRM::SetErrorHandlerMode");
}
/* ---------------------------------------------------------------------- */
IRM_RESULT
PhreeqcRM::SetErrorOn(bool t)
/* ---------------------------------------------------------------------- */
{
	this->phreeqcrm_error_string.clear();
	if (mpi_myself == 0)
	{
		this->phreeqcrm_io->Set_error_on(t);
		for (int w = 0; w < nthreads + 2; w++)
		{
			workers[w]->Set_error_on(t);
		}
	}
	return IRM_OK;
}
/* ---------------------------------------------------------------------- */
IRM_RESULT
PhreeqcRM::SetFilePrefix(const std::string & prefix)
/* ---------------------------------------------------------------------- */
{
	this->phreeqcrm_error_string.clear();
#ifdef USE_MPI
	if (this->mpi_myself == 0)
	{
		if (this->file_prefix == prefix) return IRM_OK;
		int method = METHOD_SETFILEPREFIX;
		MPI_Bcast(&method, 1, MPI_INT, 0, phreeqcrm_comm);
	}
#endif
	IRM_RESULT return_value = IRM_OK;
	if (this->mpi_myself == 0)
	{
		this->file_prefix = prefix;
	}
#ifdef USE_MPI
	int l1 = 0;
	if (mpi_myself == 0)
	{
		l1 = (int) this->file_prefix.size();
	}
	MPI_Bcast(&l1, 1, MPI_INT, 0, phreeqcrm_comm);
	this->file_prefix.resize(l1);
	MPI_Bcast((void *) this->file_prefix.c_str(), l1, MPI_CHAR, 0, phreeqcrm_comm);
#endif
	if (this->file_prefix.size() == 0)
	{
		return_value = IRM_INVALIDARG;
	}
	return this->ReturnHandler(return_value, "PhreeqcRM::SetFilePrefix");
}
/* ---------------------------------------------------------------------- */
IRM_RESULT
PhreeqcRM::SetGeneric(const std::vector<double> &source, std::vector<double> &destination_root, std::vector<double> &destination_worker, int mpiMethod, const std::string &name)
/* ---------------------------------------------------------------------- */
{
	IRM_RESULT return_value = IRM_OK;
	try
	{		
		if (mpi_myself == 0)
		{
			if ((int) source.size() < this->nxyz)
			{
				this->ErrorHandler(IRM_INVALIDARG, "Wrong number of elements in vector argument for " + name);
			}
			destination_root = source;
		}
#ifdef USE_MPI
		if (this->mpi_myself == 0)
		{
			MPI_Bcast(&mpiMethod, 1, MPI_INT, 0, phreeqcrm_comm);
		}
		ScatterNchem(destination_root, destination_worker);
#endif

	}
	catch (...)
	{
		return_value = IRM_FAIL;
	}
	return return_value;
}
/* ---------------------------------------------------------------------- */
IRM_RESULT
PhreeqcRM::SetMpiWorkerCallbackC(int (*fcn)(int *method, void *cookie))
/* ---------------------------------------------------------------------- */
{
	this->phreeqcrm_error_string.clear();
	this->mpi_worker_callback_c = fcn;
	return IRM_OK;
}
/* ---------------------------------------------------------------------- */
IRM_RESULT
PhreeqcRM::SetMpiWorkerCallbackCookie( void *cookie)
/* ---------------------------------------------------------------------- */
{
	this->phreeqcrm_error_string.clear();
	this->mpi_worker_callback_cookie = cookie;
	return IRM_OK;
}
/* ---------------------------------------------------------------------- */
IRM_RESULT
PhreeqcRM::SetMpiWorkerCallbackFortran(int (*fcn)(int *method))
/* ---------------------------------------------------------------------- */
{
	this->phreeqcrm_error_string.clear();
	this->mpi_worker_callback_fortran = fcn;
	return IRM_OK;
}
/* ---------------------------------------------------------------------- */
IRM_RESULT
PhreeqcRM::SetNthSelectedOutput(int n)
/* ---------------------------------------------------------------------- */
{
	this->phreeqcrm_error_string.clear();
	int return_value = IRM_INVALIDARG;
	if (n >= 0)
	{
		int n_user = this->workers[0]->GetNthSelectedOutputUserNumber(n);
		if (n_user >= 0)
		{
			return_value = this->workers[0]->SetCurrentSelectedOutputUserNumber(n_user);
		}
	}
	return this->ReturnHandler(PhreeqcRM::Int2IrmResult(return_value, false), "PhreeqcRM::SetNthSelectedOutput");
}
/* ---------------------------------------------------------------------- */
IRM_RESULT
PhreeqcRM::SetPartitionUZSolids(bool tf)
/* ---------------------------------------------------------------------- */
{
	this->phreeqcrm_error_string.clear();
#ifdef USE_MPI
	if (this->mpi_myself == 0)
	{
		if (this->partition_uz_solids == tf) return IRM_OK;
		int method = METHOD_SETPARTITIONUZSOLIDS;
		MPI_Bcast(&method, 1, MPI_INT, 0, phreeqcrm_comm);
	}
#endif
	if (mpi_myself == 0)
	{
		this->partition_uz_solids = tf;
	}
#ifdef USE_MPI
	int temp_tf = this->partition_uz_solids ? 1 : 0;
	MPI_Bcast(&temp_tf, 1, MPI_INT, 0, phreeqcrm_comm);
	this->partition_uz_solids = (temp_tf == 0) ? false : true;
	if (this->partition_uz_solids) 
	{
		if (this->mpi_myself == 0 && ((int) this->old_saturation_root.size() != this->nxyz))
		{
			this->old_saturation_root.resize(this->nxyz, 1.0);
		}
		ScatterNchem(old_saturation_root, old_saturation_worker);
	}
#else
	if (this->partition_uz_solids && ((int) this->old_saturation_root.size() != this->nxyz))
	{
		this->old_saturation_root.resize(this->nxyz, 1.0);
	}
#endif

	return IRM_OK;
}

/* ---------------------------------------------------------------------- */
IRM_RESULT
PhreeqcRM::SetPorosity(const std::vector<double> &t)
/* ---------------------------------------------------------------------- */
{
	this->phreeqcrm_error_string.clear();
	std::string methodName = "SetPorosity";
	IRM_RESULT result_value = SetGeneric(t, this->porosity_root, porosity_worker, METHOD_SETPOROSITY, methodName);
	this->UpdateBMI(RMVARS::Porosity);
	return this->ReturnHandler(result_value, "PhreeqcRM::" + methodName);
}

/* ---------------------------------------------------------------------- */
IRM_RESULT
PhreeqcRM::SetPressure(const std::vector<double> &t)
/* ---------------------------------------------------------------------- */
{
	this->phreeqcrm_error_string.clear();
	IRM_RESULT return_value = IRM_OK;
	try
	{

		this->phreeqcrm_error_string.clear();
		std::string methodName = "SetPressure";
		return_value = SetGeneric(t, this->pressure_root, pressure_worker, METHOD_SETPRESSURE, methodName);
	    if (return_value == IRM_OK)
		{
#ifdef USE_MPI
		for (int j = this->start_cell[this->mpi_myself]; j <= this->end_cell[this->mpi_myself]; j++)
		{
			// j is count_chem number
			int i = j - this->start_cell[this->mpi_myself];
			cxxSolution *soln_ptr = this->GetWorkers()[0]->Get_solution(j);
			if (soln_ptr)
			{
				soln_ptr->Set_patm(this->pressure_worker[i]);
			}
			cxxGasPhase *gas_ptr = this->GetWorkers()[0]->Get_gas_phase(j);
			if (gas_ptr && gas_ptr->Get_type() == cxxGasPhase::GP_PRESSURE)
			{
				gas_ptr->Set_total_p(this->pressure_worker[i]);
			}
		}
#else
#ifdef USE_OPENMP
	omp_set_num_threads(this->nthreads);
#pragma omp parallel
#pragma omp for
#endif
		for (int n = 0; n < nthreads; n++)
		{
			for (int j = this->start_cell[n]; j <= this->end_cell[n]; j++)
			{
				// j is count_chem number
				int i = this->backward_mapping[j][0];
				cxxSolution *soln_ptr = this->GetWorkers()[n]->Get_solution(j);
				if (soln_ptr)
				{
					soln_ptr->Set_patm(pressure_root[i]);
				}
				cxxGasPhase *gas_ptr = this->GetWorkers()[n]->Get_gas_phase(j);
				if (gas_ptr && gas_ptr->Get_type() == cxxGasPhase::GP_PRESSURE)
				{
					gas_ptr->Set_total_p(pressure_root[i]);  
				}
			}
		}
#endif
	}
	}
	catch (...)
	{
		return_value = IRM_FAIL;
	}
	this->UpdateBMI(RMVARS::Pressure);
	return this->ReturnHandler(return_value, "PhreeqcRM::SetPressure");
}
/* ---------------------------------------------------------------------- */
IRM_RESULT
PhreeqcRM::SetPrintChemistryOn(bool worker, bool ip, bool utility)
/* ---------------------------------------------------------------------- */
{
	this->phreeqcrm_error_string.clear();
#ifdef USE_MPI
	if (this->mpi_myself == 0)
	{
		if (this->print_chemistry_on[0] == worker &&
			this->print_chemistry_on[1] == ip &&
			this->print_chemistry_on[2] == utility)
		{
			return IRM_OK;
		}
		int method = METHOD_SETPRINTCHEMISTRYON;
		MPI_Bcast(&method, 1, MPI_INT, 0, phreeqcrm_comm);
	}
#endif
	std::vector<int> l;
	l.resize(3);
	if (mpi_myself == 0)
	{
		l[0] = worker ? 1 : 0;
		l[1] = ip ? 1 : 0;
		l[2] = utility ? 1 : 0;
	}
#ifdef USE_MPI
	MPI_Bcast(&l.front(), 3, MPI_INT, 0, phreeqcrm_comm);
#endif
	this->print_chemistry_on[0] = l[0] != 0;
	this->print_chemistry_on[1] = l[1] != 0;
	this->print_chemistry_on[2] = l[2] != 0;
	return IRM_OK;
}

/* ---------------------------------------------------------------------- */
IRM_RESULT
PhreeqcRM::SetPrintChemistryMask(const std::vector<int> & m)
/* ---------------------------------------------------------------------- */
{
	this->phreeqcrm_error_string.clear();
	IRM_RESULT return_value = IRM_OK;
	try
	{
		if (this->mpi_myself == 0)
		{
#ifdef USE_MPI
			int method = METHOD_SETPRINTCHEMISTRYMASK;
			MPI_Bcast(&method, 1, MPI_INT, 0, phreeqcrm_comm);
#endif
			if ((int) m.size() < this->nxyz)
			{				
				this->ErrorHandler(IRM_INVALIDARG, "Wrong number of elements in vector argument for SetPrintChemistryMask");
			}
			this->print_chem_mask_root = m;
		}
#ifdef USE_MPI
		ScatterNchem(print_chem_mask_root, print_chem_mask_worker);
#endif
	}
	catch (...)
	{
		return_value = IRM_INVALIDARG;
	}
	return this->ReturnHandler(return_value, "PhreeqcRM::SetPrintChemistryMask");
}
/* ---------------------------------------------------------------------- */
IRM_RESULT
PhreeqcRM::SetRebalanceByCell(bool t)
/* ---------------------------------------------------------------------- */
{
	this->phreeqcrm_error_string.clear();
#ifdef USE_MPI
	if (this->mpi_myself == 0)
	{
		if (this->rebalance_by_cell == t) return IRM_OK;
		int method = METHOD_SETREBALANCEBYCELL;
		MPI_Bcast(&method, 1, MPI_INT, 0, phreeqcrm_comm);
	}
#endif
	if (mpi_myself == 0)
	{
		this->rebalance_by_cell = t;
	}
#ifdef USE_MPI
	int temp_tf = this->rebalance_by_cell ? 1 : 0;
	MPI_Bcast(&temp_tf, 1, MPI_INT, 0, phreeqcrm_comm);
	this->rebalance_by_cell = (temp_tf == 0) ? false : true;
	//MPI_Bcast(&(this->rebalance_by_cell), 1, MPI_LOGICAL, 0, phreeqcrm_comm);
#endif
	return IRM_OK;
}
/* ---------------------------------------------------------------------- */
IRM_RESULT
PhreeqcRM::SetRebalanceFraction(double t)
/* ---------------------------------------------------------------------- */
{
	this->phreeqcrm_error_string.clear();
	if (mpi_myself == 0)
	{
		if (this->rebalance_fraction == t) return IRM_OK;
		this->rebalance_fraction = t;
	}
#ifdef USE_MPI
	if (this->mpi_myself == 0)
	{
		int method = METHOD_SETREBALANCEFRACTION;
		MPI_Bcast(&method, 1, MPI_INT, 0, phreeqcrm_comm);
	}
	MPI_Bcast(&(this->rebalance_fraction), 1, MPI_DOUBLE, 0, phreeqcrm_comm);
#endif
	return IRM_OK;
}

/* ---------------------------------------------------------------------- */
IRM_RESULT
PhreeqcRM::SetRepresentativeVolume(const std::vector<double> &t)
/* ---------------------------------------------------------------------- */
{
	this->phreeqcrm_error_string.clear();
	std::string methodName = "SetRepresentativeVolume";
	IRM_RESULT result_value = SetGeneric(t, this->rv_root, rv_worker, METHOD_SETREPRESENTATIVEVOLUME, methodName);
	return this->ReturnHandler(result_value, "PhreeqcRM::" + methodName);
}

/* ---------------------------------------------------------------------- */
IRM_RESULT
PhreeqcRM::SetSaturation(const std::vector<double>& t)
/* ---------------------------------------------------------------------- */
{
	// For backward compatibility
	return SetSaturationUser(t);
}

/* ---------------------------------------------------------------------- */
IRM_RESULT
PhreeqcRM::SetSaturationUser(const std::vector<double> &t)
/* ---------------------------------------------------------------------- */
{
	this->phreeqcrm_error_string.clear();
	std::string methodName = "SetSaturationUser";
	IRM_RESULT result_value = SetGeneric(t, this->saturation_root, saturation_worker, METHOD_SETSATURATIONUSER, methodName);
	this->UpdateBMI(RMVARS::SaturationUser);
	return this->ReturnHandler(result_value, "PhreeqcRM::" + methodName);
}
/* ---------------------------------------------------------------------- */
IRM_RESULT
PhreeqcRM::SetScreenOn(bool t)
/* ---------------------------------------------------------------------- */
{
	this->phreeqcrm_error_string.clear();
	if (mpi_myself == 0)
	{
		this->phreeqcrm_io->Set_screen_on(t);
		for (int w = 0; w < nthreads + 2; w++)
		{
			workers[w]->Set_screen_on(t);
		}
	}
	return IRM_OK;
}
/* ---------------------------------------------------------------------- */
IRM_RESULT
PhreeqcRM::SetSelectedOutputOn(bool t)
/* ---------------------------------------------------------------------- */
{
	this->phreeqcrm_error_string.clear();
#ifdef USE_MPI
	if (this->mpi_myself == 0)
	{
		if (this->selected_output_on == t) return IRM_OK;
		int method = METHOD_SETSELECTEDOUTPUTON;
		MPI_Bcast(&method, 1, MPI_INT, 0, phreeqcrm_comm);
	}
#endif
	if (mpi_myself == 0)
	{
		this->selected_output_on = t;
	}
#ifdef USE_MPI
	int temp_tf = this->selected_output_on ? 1 : 0;
	MPI_Bcast(&temp_tf, 1, MPI_INT, 0, phreeqcrm_comm);
	this->selected_output_on = (temp_tf == 0) ? false : true;
	//MPI_Bcast(&this->selected_output_on, 1, MPI_LOGICAL, 0, phreeqcrm_comm);
#endif
	this->UpdateBMI(RMVARS::SelectedOutputOn);
	return IRM_OK;
}

/* ---------------------------------------------------------------------- */
IRM_RESULT
PhreeqcRM::SetSpeciesSaveOn(bool t)
/* ---------------------------------------------------------------------- */
{
	this->phreeqcrm_error_string.clear();
#ifdef USE_MPI
	if (this->mpi_myself == 0)
	{
		if (this->species_save_on == t) return IRM_OK;
		int method = METHOD_SETSPECIESSAVEON;
		MPI_Bcast(&method, 1, MPI_INT, 0, phreeqcrm_comm);
	}
#endif
	if (mpi_myself == 0)
	{
		this->species_save_on = t;
	}
#ifdef USE_MPI
	int temp_tf = this->species_save_on ? 1 : 0;
	MPI_Bcast(&temp_tf, 1, MPI_INT, 0, phreeqcrm_comm);
	this->species_save_on = (temp_tf == 0) ? false : true;
	//MPI_Bcast(&this->species_save_on, 1, MPI_LOGICAL, 0, phreeqcrm_comm);
#endif
	for (int i = 0; i < this->nthreads + 1; i++)
	{
		this->workers[i]->PhreeqcPtr->save_species = this->species_save_on;
	}

	return IRM_OK;
}
/* ---------------------------------------------------------------------- */
IRM_RESULT
PhreeqcRM::SetTemperature(const std::vector<double> &t)
/* ---------------------------------------------------------------------- */
{
	this->phreeqcrm_error_string.clear();
	IRM_RESULT return_value = IRM_OK;
	try
	{
		std::string methodName = "SetTemperature";
		return_value = SetGeneric(t, this->tempc_root, tempc_worker, METHOD_SETTEMPERATURE, methodName);
		if (return_value == IRM_OK)
		{

#ifdef USE_MPI
		for (int j = this->start_cell[this->mpi_myself]; j <= this->end_cell[this->mpi_myself]; j++)
		{
			// j is count_chem number
			int i = j - this->start_cell[this->mpi_myself];
			cxxSolution *soln_ptr = this->GetWorkers()[0]->Get_solution(j);
			if (soln_ptr)
			{
				soln_ptr->Set_tc(tempc_worker[i]);
			}
		}
#else
#ifdef USE_OPENMP
	omp_set_num_threads(this->nthreads);
#pragma omp parallel
#pragma omp for
#endif
		for (int n = 0; n < nthreads; n++)
		{
			for (int j = this->start_cell[n]; j <= this->end_cell[n]; j++)
			{
				// j is count_chem number
				int i = this->backward_mapping[j][0];
				cxxSolution *soln_ptr = this->GetWorkers()[n]->Get_solution(j);
				if (soln_ptr)
				{
					soln_ptr->Set_tc(tempc_root[i]);
				}
			}
		}
#endif
		}
	}
	catch (...)
	{
		return_value = IRM_FAIL;
	}
	this->UpdateBMI(RMVARS::Temperature);
	return this->ReturnHandler(return_value, "PhreeqcRM::SetTemperature");
}
/* ---------------------------------------------------------------------- */
IRM_RESULT
PhreeqcRM::SetTime(double t)
/* ---------------------------------------------------------------------- */
{
	this->phreeqcrm_error_string.clear();
#ifdef USE_MPI
	if (this->mpi_myself == 0)
	{
		if (this->time == t) return IRM_OK;
		int method = METHOD_SETTIME;
		MPI_Bcast(&method, 1, MPI_INT, 0, phreeqcrm_comm);
	}
#endif
	if (mpi_myself == 0)
	{
		this->time = t;
	}
#ifdef USE_MPI
	MPI_Bcast(&this->time, 1, MPI_DOUBLE, 0, phreeqcrm_comm);
#endif
	this->UpdateBMI(RMVARS::Time);
	return IRM_OK;
}

/* ---------------------------------------------------------------------- */
IRM_RESULT
PhreeqcRM::SetTimeConversion(double t)
/* ---------------------------------------------------------------------- */
{
	this->phreeqcrm_error_string.clear();
#ifdef USE_MPI
	if (this->mpi_myself == 0)
	{
		if (this->time_conversion == t) return IRM_OK;
		int method = METHOD_SETTIMECONVERSION;
		MPI_Bcast(&method, 1, MPI_INT, 0, phreeqcrm_comm);
	}
#endif
	if (mpi_myself == 0)
	{
		this->time_conversion = t;
	}
#ifdef USE_MPI
	MPI_Bcast(&this->time_conversion, 1, MPI_DOUBLE, 0, phreeqcrm_comm);
#endif
	return IRM_OK;
}

/* ---------------------------------------------------------------------- */
IRM_RESULT
PhreeqcRM::SetTimeStep(double t)
/* ---------------------------------------------------------------------- */
{
	this->phreeqcrm_error_string.clear();
#ifdef USE_MPI
	if (this->mpi_myself == 0)
	{
		if (this->time_step == t) return IRM_OK;
		int method = METHOD_SETTIMESTEP;
		MPI_Bcast(&method, 1, MPI_INT, 0, phreeqcrm_comm);
	}
#endif
	if (this->mpi_myself == 0)
	{
		this->time_step = t;
	}
#ifdef USE_MPI
	MPI_Bcast(&this->time_step, 1, MPI_DOUBLE, 0, phreeqcrm_comm);
#endif
	this->UpdateBMI(RMVARS::TimeStep);
	return IRM_OK;
}
/* ---------------------------------------------------------------------- */
IRM_RESULT
PhreeqcRM::SetUnitsExchange(int u)
/* ---------------------------------------------------------------------- */
{
	this->phreeqcrm_error_string.clear();
#ifdef USE_MPI
	if (this->mpi_myself == 0)
	{
		if (this->units_Exchange  == u) return IRM_OK;
		int method = METHOD_SETUNITSEXCHANGE;
		MPI_Bcast(&method, 1, MPI_INT, 0, phreeqcrm_comm);
	}
#endif
	IRM_RESULT return_value = IRM_OK;
	if (mpi_myself == 0)
	{
		if (u >= 0 && u < 3)
		{
			this->units_Exchange  = u;
		}
		else
		{
			return_value = IRM_INVALIDARG;
		}
	}
#ifdef USE_MPI
	MPI_Bcast(&this->units_Exchange,  1, MPI_INT, 0, phreeqcrm_comm);
#endif
	return this->ReturnHandler(return_value, "PhreeqcRM::SetUnitsExchange");
}
/* ---------------------------------------------------------------------- */
IRM_RESULT
PhreeqcRM::SetUnitsGasPhase(int u)
/* ---------------------------------------------------------------------- */
{
	this->phreeqcrm_error_string.clear();
#ifdef USE_MPI
	if (this->mpi_myself == 0)
	{
		if (this->units_GasPhase  == u) return IRM_OK;
		int method = METHOD_SETUNITSGASPHASE;
		MPI_Bcast(&method, 1, MPI_INT, 0, phreeqcrm_comm);
	}
#endif
	IRM_RESULT return_value = IRM_OK;
	if (mpi_myself == 0)
	{
		if (u >= 0 && u < 3)
		{
			this->units_GasPhase  = u;
		}
		else
		{
			return_value = IRM_INVALIDARG;
		}
	}
#ifdef USE_MPI
	MPI_Bcast(&this->units_GasPhase,  1, MPI_INT, 0, phreeqcrm_comm);
#endif
	return this->ReturnHandler(return_value, "PhreeqcRM::SetUnitsGasPhase");
}
/* ---------------------------------------------------------------------- */
IRM_RESULT
PhreeqcRM::SetUnitsKinetics(int u)
/* ---------------------------------------------------------------------- */
{
	this->phreeqcrm_error_string.clear();
#ifdef USE_MPI
	if (this->mpi_myself == 0)
	{
		if (this->units_Kinetics  == u) return IRM_OK;
		int method = METHOD_SETUNITSKINETICS;
		MPI_Bcast(&method, 1, MPI_INT, 0, phreeqcrm_comm);
	}
#endif
	IRM_RESULT return_value = IRM_OK;
	if (mpi_myself == 0)
	{
		if (u >= 0 && u < 3)
		{
			this->units_Kinetics  = u;
		}
		else
		{
			return_value = IRM_INVALIDARG;
		}
	}
#ifdef USE_MPI
	MPI_Bcast(&this->units_Kinetics,  1, MPI_INT, 0, phreeqcrm_comm);
#endif
	return this->ReturnHandler(return_value, "PhreeqcRM::SetUnitsKinetics");
}
/* ---------------------------------------------------------------------- */
IRM_RESULT
PhreeqcRM::SetUnitsPPassemblage(int u)
/* ---------------------------------------------------------------------- */
{
	this->phreeqcrm_error_string.clear();
#ifdef USE_MPI
	if (this->mpi_myself == 0)
	{
		if (this->units_PPassemblage  == u) return IRM_OK;
		int method = METHOD_SETUNITSPPASSEMBLAGE;
		MPI_Bcast(&method, 1, MPI_INT, 0, phreeqcrm_comm);
	}
#endif
	IRM_RESULT return_value = IRM_OK;
	if (mpi_myself == 0)
	{
		if (u >= 0 && u < 3)
		{
			this->units_PPassemblage  = u;
		}
		else
		{
			return_value = IRM_INVALIDARG;
		}
	}
#ifdef USE_MPI
	MPI_Bcast(&this->units_PPassemblage,  1, MPI_INT, 0, phreeqcrm_comm);
#endif
	return this->ReturnHandler(return_value, "PhreeqcRM::SetUnitsPPassemblage");
}
/* ---------------------------------------------------------------------- */
IRM_RESULT
PhreeqcRM::SetUnitsSolution(int u)
/* ---------------------------------------------------------------------- */
{
	this->phreeqcrm_error_string.clear();
#ifdef USE_MPI
	if (this->mpi_myself == 0)
	{
		if (this->units_Solution == u) return IRM_OK;
		int method = METHOD_SETUNITSSOLUTION;
		MPI_Bcast(&method, 1, MPI_INT, 0, phreeqcrm_comm);
	}
#endif
	IRM_RESULT return_value = IRM_OK;
	if (mpi_myself == 0)
	{
		if (u > 0 && u < 4)
		{
			this->units_Solution  = u;
		}
		else
		{
			return_value = IRM_INVALIDARG;
		}
	}
#ifdef USE_MPI
	MPI_Bcast(&this->units_Solution,  1, MPI_INT, 0, phreeqcrm_comm);
#endif
	return this->ReturnHandler(return_value, "PhreeqcRM::SetUnitsSolution");
}
/* ---------------------------------------------------------------------- */
IRM_RESULT
PhreeqcRM::SetUnitsSSassemblage(int u)
/* ---------------------------------------------------------------------- */
{
	this->phreeqcrm_error_string.clear();
#ifdef USE_MPI
	if (this->mpi_myself == 0)
	{
		if (this->units_SSassemblage == u) return IRM_OK;
		int method = METHOD_SETUNITSSSASSEMBLAGE;
		MPI_Bcast(&method, 1, MPI_INT, 0, phreeqcrm_comm);
	}
#endif
	IRM_RESULT return_value = IRM_OK;
	if (mpi_myself == 0)
	{
		if (u >= 0 && u < 3)
		{
			this->units_SSassemblage  = u;
		}
		else
		{
			return_value = IRM_INVALIDARG;
		}
	}
#ifdef USE_MPI
	MPI_Bcast(&this->units_SSassemblage,  1, MPI_INT, 0, phreeqcrm_comm);
#endif
	return this->ReturnHandler(return_value, "PhreeqcRM::SetUnitsSSassemblage");
}
/* ---------------------------------------------------------------------- */
IRM_RESULT
PhreeqcRM::SetUnitsSurface(int u)
/* ---------------------------------------------------------------------- */
{
	this->phreeqcrm_error_string.clear();
#ifdef USE_MPI
	if (this->mpi_myself == 0)
	{
		if (this->units_Surface == u) return IRM_OK;
		int method = METHOD_SETUNITSSURFACE;
		MPI_Bcast(&method, 1, MPI_INT, 0, phreeqcrm_comm);
	}
#endif
	IRM_RESULT return_value = IRM_OK;
	if (mpi_myself == 0)
	{
		if (u >= 0 && u < 3)
		{
			this->units_Surface  = u;
		}
		else
		{
			return_value = IRM_INVALIDARG;
		}
	}
#ifdef USE_MPI
	MPI_Bcast(&this->units_Surface,  1, MPI_INT, 0, phreeqcrm_comm);
#endif
	return this->ReturnHandler(return_value, "PhreeqcRM::SetUnitsSurface");
}
/* ---------------------------------------------------------------------- */
IRM_RESULT
PhreeqcRM::SpeciesConcentrations2Module(const std::vector<double> & species_conc_in)
/* ---------------------------------------------------------------------- */
{
	this->phreeqcrm_error_string.clear();
	std::vector<double> species_conc = species_conc_in;
#ifdef USE_MPI
	if (this->mpi_myself == 0)
	{
		int method = METHOD_SPECIESCONCENTRATIONS2MODULE;
		MPI_Bcast(&method, 1, MPI_INT, 0, phreeqcrm_comm);
	}
#endif
	if (this->species_save_on)
	{
#ifdef USE_MPI
		if (this->mpi_myself > 0)
		{
			species_conc.resize(this->species_names.size() * this->nxyz, 0.0);
		}
		MPI_Bcast(&species_conc.front(), (int) this->species_names.size() * nxyz, MPI_DOUBLE, 0, phreeqcrm_comm);
		for (int n = this->mpi_myself; n < this->mpi_myself + 1; n++)
#else
 		for (int n = 0; n < this->nthreads; n++)
#endif
		{
			for (int i = this->start_cell[n]; i <= this->end_cell[n]; i++)
			{
				int j = this->backward_mapping[i][0];   // user grid number
				cxxNameDouble solution_totals;
				for (size_t k = 0; k < this->components.size(); k++)
				{
					solution_totals.add(components[k].c_str(), 0.0);
				}
				for (size_t k = 0; k < this->species_names.size(); k++)
				{
					// kth species, jth cell
					double conc = species_conc[k * this->nxyz + j];
					cxxNameDouble::iterator it = this->species_stoichiometry[k].begin();
					for ( ; it != this->species_stoichiometry[k].end(); it++)
					{
						solution_totals.add(it->first.c_str(), it->second * conc);
					}
				}
				cxxNameDouble nd;
				std::vector<double> d;
				d.resize(3,0.0);
#ifdef USE_MPI
				int l = i - this->start_cell[n];
				solution_totals.multiply(this->porosity_worker[l] * this->saturation_worker[l] * this->rv_worker[l]);
#else
				solution_totals.multiply(this->porosity_root[j] * this->saturation_root[j] * this->rv_root[j]);
#endif
				cxxNameDouble::iterator it = solution_totals.begin();
				for ( ; it != solution_totals.end(); it++)
				{
					if (it->first == "H")
					{
						d[0] = it->second;
					}
					else if (it->first == "O")
					{
						d[1] = it->second;
					}
					else if (it->first == "Charge")
					{
						d[2] = it->second;
					}
					else
					{
						nd.add(it->first.c_str(), it->second);
					}
				}
#ifdef USE_MPI
				cxxSolution *soln_ptr = this->GetWorkers()[0]->Get_solution(i);
#else
				cxxSolution *soln_ptr = this->GetWorkers()[n]->Get_solution(i);
#endif
				if (soln_ptr)
				{
					soln_ptr->Update(d[0], d[1], d[2], nd);
				}
			}
		}
		return IRM_OK;
	}
	return IRM_INVALIDARG;
}
#ifdef NEVER_GOT_WORKING_TO_ELIMINATE_BACKWARD_MAPPING
/* ---------------------------------------------------------------------- */
IRM_RESULT
PhreeqcRM::SpeciesConcentrations2Module(std::vector<double> & species_conc_root)
/* ---------------------------------------------------------------------- */
{
	this->phreeqcrm_error_string.clear();

	if (this->mpi_myself == 0)
	{
		int method = METHOD_SPECIESCONCENTRATIONS2MODULE;
		MPI_Bcast(&method, 1, MPI_INT, 0, phreeqcrm_comm);
	}
	if (this->species_save_on)
	{
		std::vector<double> species_conc_worker;
		if (this->mpi_myself > 0)
		{
			species_conc_root.resize(this->species_names.size() * this->nxyz, 0.0);
		}
		for (size_t i = 0; i < species_names.size(); i++)
		{
			std::vector<double> spec_root;
			if (this->mpi_myself == 0)
			{
				spec_root.resize(this->nxyz);
				memcpy(&(spec_root[0]), &(species_conc_root[i * this->nxyz]), (size_t) this->nxyz * sizeof(double));
			}
			std::vector<double> spec_worker;
			int size = this->end_cell[this->mpi_myself] - this->start_cell[mpi_myself] + 1;
			spec_worker.resize(size);
			ScatterNchem(spec_root, spec_worker);
			species_conc_worker.insert(species_conc_worker.end(), spec_worker.begin(), spec_worker.end());
		}
			for (int i = this->start_cell[this->mpi_myself]; i <= this->end_cell[this->mpi_myself]; i++)
			{
				//int j = this->backward_mapping_root[i][0];   // user grid number
				cxxNameDouble solution_totals;
				for (size_t k = 0; k < this->components.size(); k++)
				{
					solution_totals.add(components[k].c_str(), 0.0);
				}
				int stride = this->end_cell[this->mpi_myself] - this->start_cell[mpi_myself] + 1;
				for (size_t k = 0; k < this->species_names.size(); k++)
				{
					// kth species, jth cell
					double conc = species_conc_worker[k * stride + i];
					cxxNameDouble::iterator it = this->species_stoichiometry[k].begin();
					for ( ; it != this->species_stoichiometry[k].end(); it++)
					{
						solution_totals.add(it->first.c_str(), it->second * conc);
					}
				}
				cxxNameDouble nd;
				std::vector<double> d;
				d.resize(3,0.0);
				int l = i - this->start_cell[this->mpi_myself];
				solution_totals.multiply(this->porosity_worker[l] * this->saturation_worker[l] * this->rv_worker[l]);

				cxxNameDouble::iterator it = solution_totals.begin();
				for ( ; it != solution_totals.end(); it++)
				{
					if (it->first == "H")
					{
						d[0] = it->second;
					}
					else if (it->first == "O")
					{
						d[1] = it->second;
					}
					else if (it->first == "Charge")
					{
						d[2] = it->second;
					}
					else
					{
						nd.add(it->first.c_str(), it->second);
					}
				}

				cxxSolution *soln_ptr = this->GetWorkers()[0]->Get_solution(i);

				if (soln_ptr)
				{
					soln_ptr->Update(d[0], d[1], d[2], nd);
				}
			}
		return IRM_OK;
	}
	return IRM_INVALIDARG;
}

#endif
IRM_RESULT PhreeqcRM::StateSave(int istate) 
{
#ifdef USE_MPI
	if (this->mpi_myself == 0)
	{
		int method = METHOD_STATESAVE;
		MPI_Bcast(&method, 1, MPI_INT, 0, phreeqcrm_comm);
	}
	MPI_Bcast(&istate, 1, MPI_INT, 0, phreeqcrm_comm);
#endif

#ifdef USE_OPENMP
	omp_set_num_threads(this->nthreads);
#pragma omp parallel
#pragma omp for
#endif
	for (int i = 0; i < nthreads; i++)
	{
		Phreeqc *phrqc_ptr = workers[i]->Get_PhreeqcPtr();
		State st;
		workers[i]->state_map[istate] = st;
		phrqc_ptr->phreeqc2cxxStorageBin(workers[i]->state_map[istate].worker_bin);
		workers[i]->state_map[istate].uz_bin = workers[i]->uz_bin;
		workers[i]->state_map[istate].start_cell = this->start_cell;
		workers[i]->state_map[istate].end_cell = this->end_cell;
	}
	return IRM_OK;
}
IRM_RESULT PhreeqcRM::StateApply(int istate) 
{
#ifdef USE_MPI
	if (this->mpi_myself == 0)
	{
		int method = METHOD_STATEAPPLY;
		MPI_Bcast(&method, 1, MPI_INT, 0, phreeqcrm_comm);
	}
	MPI_Bcast(&istate, 1, MPI_INT, 0, phreeqcrm_comm);
#endif
	if (workers[0]->state_map.find(istate) == workers[0]->state_map.end())
	{
		return IRM_INVALIDARG;
	}
	this->start_cell = workers[0]->state_map[istate].start_cell;
	this->end_cell = workers[0]->state_map[istate].end_cell;
#ifdef USE_OPENMP
	omp_set_num_threads(this->nthreads);
#pragma omp parallel
#pragma omp for
#endif
	for (int i = 0; i < nthreads; i++)
	{
		Phreeqc* phrqc_ptr = workers[i]->Get_PhreeqcPtr();
		phrqc_ptr->Get_Rxn_solution_map() = workers[i]->state_map[istate].worker_bin.Get_Solutions();
		phrqc_ptr->Get_Rxn_exchange_map() = workers[i]->state_map[istate].worker_bin.Get_Exchangers();
		phrqc_ptr->Get_Rxn_gas_phase_map() = workers[i]->state_map[istate].worker_bin.Get_GasPhases();
		phrqc_ptr->Get_Rxn_kinetics_map() = workers[i]->state_map[istate].worker_bin.Get_Kinetics();
		phrqc_ptr->Get_Rxn_pp_assemblage_map() = workers[i]->state_map[istate].worker_bin.Get_PPassemblages();
		phrqc_ptr->Get_Rxn_ss_assemblage_map() = workers[i]->state_map[istate].worker_bin.Get_SSassemblages();
		phrqc_ptr->Get_Rxn_surface_map() = workers[i]->state_map[istate].worker_bin.Get_Surfaces();
		phrqc_ptr->Get_Rxn_mix_map() = workers[i]->state_map[istate].worker_bin.Get_Mixes();
		phrqc_ptr->Get_Rxn_reaction_map() = workers[i]->state_map[istate].worker_bin.Get_Reactions();
		phrqc_ptr->Get_Rxn_temperature_map() = workers[i]->state_map[istate].worker_bin.Get_Temperatures();
		phrqc_ptr->Get_Rxn_pressure_map() = workers[i]->state_map[istate].worker_bin.Get_Pressures();
		workers[i]->uz_bin = workers[i]->state_map[istate].uz_bin;
		workers[i]->start_cell = workers[i]->state_map[istate].start_cell[i];
		workers[i]->end_cell = workers[i]->state_map[istate].end_cell[i];
	}
	return IRM_OK;
}
IRM_RESULT PhreeqcRM::StateDelete(int istate) 
{
#ifdef USE_MPI
	if (this->mpi_myself == 0)
	{
		int method = METHOD_STATEDELETE;
		MPI_Bcast(&method, 1, MPI_INT, 0, phreeqcrm_comm);
	}
	MPI_Bcast(&istate, 1, MPI_INT, 0, phreeqcrm_comm);
#endif
	if (workers[0]->state_map.find(istate) == workers[0]->state_map.end())
	{
		return IRM_INVALIDARG;
	}
#ifdef USE_OPENMP
	omp_set_num_threads(this->nthreads);
#pragma omp parallel
#pragma omp for
#endif
	for (int i = 0; i < nthreads; i++)
	{
		workers[i]->state_map.erase(istate);
	}
	return IRM_OK;
}
/* ---------------------------------------------------------------------- */
double
PhreeqcRM::TimeStandardTask()
/* ---------------------------------------------------------------------- */
{
	double a = 0.0;
	double count = 0.0;
	double t0 = (double) CLOCK();
	for (;;)
	{
		for (int i = 1; i < 1000; i++)
		{
			count += 1.0;
			a += 1.0/sqrt(count + a);
		}
		if (((double) CLOCK() - t0) > 1.0) break;
	}
	return count;
}
#ifdef USE_MPI
/* ---------------------------------------------------------------------- */
IRM_RESULT
PhreeqcRM::TransferCells(cxxStorageBin &t_bin, int old, int nnew)
/* ---------------------------------------------------------------------- */
{
	// Throws on error
	IRM_RESULT return_value = IRM_OK;
	try
	{
		if (this->mpi_myself == old)
		{
			// make raw_stream for transfer
			std::ostringstream raw_stream;
			t_bin.dump_raw(raw_stream, 0);
			size_t string_size = raw_stream.str().size() + 1;
//#ifdef USE_GZ
//			// compress string into deflated
//			char * deflated = new char[string_size];
//			uLongf deflated_size = (uLongf) string_size;
//			compress((Bytef *)deflated, (uLongf *) &deflated_size, (const Bytef *) raw_stream.str().c_str(), (uLongf) string_size);
//
//			// transfer sizes and compressed string to new process
//			int size[2];
//			size[0] = (int) deflated_size;
//			size[1] = (int) string_size;
//			MPI_Send(&size, 2, MPI_INT, nnew, 0, phreeqcrm_comm);
//			MPI_Send((void *) deflated, size[0], MPI_BYTE, nnew, 0, phreeqcrm_comm);
//
//			// delete work space
//			delete deflated;
//#else
			MPI_Send(&string_size, 1, MPI_INT, nnew, 0, phreeqcrm_comm);
			MPI_Send((void *) raw_stream.str().c_str(), (int) string_size, MPI_CHAR, nnew, 0, phreeqcrm_comm);
//#endif

		}
		else if (this->mpi_myself == nnew)
		{
			MPI_Status mpi_status;

//#ifdef USE_GZ
//			// Recieve sizes and compressed string
//			int size[2];
//			MPI_Recv(&size, 2, MPI_INT, old, 0, phreeqcrm_comm, &mpi_status);
//			char *deflated = new char[size[0]];
//			MPI_Recv((void *) deflated, size[0], MPI_BYTE, old, 0, phreeqcrm_comm, &mpi_status);
//
//			// uncompress string into string_buffer
//			char * string_buffer = new char[size[1]];
//			uLongf uncompressed_length = (uLongf) size[1];
//			uncompress((Bytef *)string_buffer, &uncompressed_length, (const Bytef *) deflated, (uLongf) size[0]);
//			delete deflated;
//
//			// RunString to add cells to module
//			IPhreeqcPhast * phast_iphreeqc_worker = this->workers[0];
//			int status = phast_iphreeqc_worker->RunString(string_buffer);
//			delete string_buffer;
//#else
			int string_size;;
			MPI_Recv(&string_size, 1, MPI_INT, old, 0, phreeqcrm_comm, &mpi_status);
			char *string_buffer = new char[string_size];
			MPI_Recv((void *) string_buffer, string_size, MPI_CHAR, old, 0, phreeqcrm_comm, &mpi_status);
			IPhreeqcPhast * phast_iphreeqc_worker = this->workers[0];
			int status = phast_iphreeqc_worker->RunString(string_buffer);
			if (status != 0)
			{
				this->ErrorMessage(phast_iphreeqc_worker->GetErrorString());
			}
			delete [] string_buffer;
//#endif
			this->ErrorHandler(PhreeqcRM::Int2IrmResult(status, false), "RunString in TransferCells");
		}
	}
	catch (...)
	{
		return_value = IRM_FAIL;
	}
	this->ErrorHandler(return_value, "PhreeqcRM::TransferCells");
	return return_value;
}
/* ---------------------------------------------------------------------- */
IRM_RESULT
PhreeqcRM::TransferCellsUZ(std::ostringstream &raw_stream, int old, int nnew)
/* ---------------------------------------------------------------------- */
{
	// Throws on error
	IRM_RESULT return_value = IRM_OK;
	try
	{
		if (this->mpi_myself == old)
		{
			size_t string_size = raw_stream.str().size() + 1;
			MPI_Send(&string_size, 1, MPI_INT, nnew, 0, phreeqcrm_comm);
			MPI_Send((void *) raw_stream.str().c_str(), (int) string_size, MPI_CHAR, nnew, 0, phreeqcrm_comm);
		}
		else if (this->mpi_myself == nnew)
		{
			MPI_Status mpi_status;
			int string_size;;
			MPI_Recv(&string_size, 1, MPI_INT, old, 0, phreeqcrm_comm, &mpi_status);
			char *string_buffer = new char[string_size];
			MPI_Recv((void *) string_buffer, string_size, MPI_CHAR, old, 0, phreeqcrm_comm, &mpi_status);
			IPhreeqcPhast * phast_iphreeqc_worker = this->workers[0];
			std::istringstream iss(string_buffer);
			delete [] string_buffer;
			CParser cp(iss);
			cp.set_echo_file(CParser::EO_NONE);
			cp.set_echo_stream(CParser::EO_NONE);
			phast_iphreeqc_worker->uz_bin.read_raw(cp);
		}
	}
	catch (...)
	{
		return_value = IRM_FAIL;
	}
	this->ErrorHandler(return_value, "PhreeqcRM::TransferCellsUZ");
	return return_value;
}

#endif
/* ---------------------------------------------------------------------- */
void
PhreeqcRM::UpdateBMI(RMVARS v_enum)
/* ---------------------------------------------------------------------- */
{
	// no-op
}
/* ---------------------------------------------------------------------- */
void
PhreeqcRM::UseSolutionDensityVolume(bool tf)
/* ---------------------------------------------------------------------- */
{
	this->phreeqcrm_error_string.clear();
#ifdef USE_MPI
	if (this->mpi_myself == 0)
	{
		if (this->use_solution_density_volume == tf) return;
		int method = METHOD_USESOLUTIONDENSITYVOLUME;
		MPI_Bcast(&method, 1, MPI_INT, 0, phreeqcrm_comm);
	}
#endif
	if (mpi_myself == 0)
	{
		this->use_solution_density_volume = tf;
	}
#ifdef USE_MPI
	int temp_tf = this->use_solution_density_volume ? 1 : 0;
	MPI_Bcast(&temp_tf, 1, MPI_INT, 0, phreeqcrm_comm);
	this->use_solution_density_volume = (temp_tf == 0) ? false : true;
	//MPI_Bcast(&this->use_solution_density_volume,  1, MPI_LOGICAL, 0, phreeqcrm_comm);
#endif
}
/* ---------------------------------------------------------------------- */
void
PhreeqcRM::WarningMessage(const std::string &str)
/* ---------------------------------------------------------------------- */
{
#ifdef USE_OPENMP
#pragma omp critical
#endif
	{
		this->phreeqcrm_io->warning_msg(str.c_str());
	}
}
/* ---------------------------------------------------------------------- */
void
PhreeqcRM::set_data_for_parallel_processing(int value)
/* ---------------------------------------------------------------------- */
{
	this->initializer->data_for_parallel_processing = value;
}
/* ---------------------------------------------------------------------- */
void
PhreeqcRM::set_nxyz(int value)
/* ---------------------------------------------------------------------- */
{
	this->initializer->nxyz_arg = value;
}
/* ---------------------------------------------------------------------- */
void
PhreeqcRM::set_io(PHRQ_io *value)
/* ---------------------------------------------------------------------- */
{
	this->initializer->io = value;
}





