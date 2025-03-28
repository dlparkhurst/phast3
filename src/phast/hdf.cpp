/*
  hdfcpp.cpp
*/
#include <string.h>
#include <stdlib.h>
#if defined(_MT)
#define _HDF5USEDLL_			/* reqd for Multithreaded run-time library (Win32) */
#endif
#include <hdf5.h>

#include <string>
#include <sstream>
#include <vector>
#include <assert.h>
#include <iostream>

#include "phrqtype.h"
#include "hdf.h"

#define PHRQ_malloc malloc
#define PHRQ_free free
#define PHRQ_calloc calloc
#define PHRQ_realloc realloc

#define OK 1
#define STOP 1
#define CONTINUE 0
#define TRUE 1
#define FALSE 0
#define EMPTY 2
#define MAX_PATH 260

#define hssize_t hsize_t
int string_trim(char *str);
void malloc_error(void);
void error_msg(const char * msg, int stop);
char error_string[1024];

/*
 *   static functions
 */
int file_exists(const char *name);
static void hdf_finalize_headings(int iso);
static hid_t open_hdf_file(int iso, const char *prefix, int prefix_l);
static void write_axis(hid_t loc_id, double *a, int na, const char *name);
static void write_proc_timestep(int iso, int rank, int cell_count,
								hid_t file_dspace_id, hid_t dset_id,
								std::vector<double> &array);
static void write_vector(int iso, hid_t loc_id, double a[], int na, const char *name);
static void write_vector_mask(int iso, hid_t loc_id, int a[], int na,
							  const char *name);

/*
 *   statics used only by process 0
 */
struct root_info
{
	hid_t hdf_file_id;
	hid_t grid_gr_id;
	hid_t features_gr_id;
	hid_t current_file_dspace_id;
	hid_t current_file_dset_id;
	hid_t current_timestep_gr_id;
	int nx;
	int ny;
	int nz;
	int nxy;
	int nxyz;
	size_t scalar_name_max_len;
	std::vector <std::string> scalar_names;
	char timestep_units[40];
	char timestep_buffer[120];
	std::vector < int > active;
	std::vector < int > natural_to_active;
	std::vector < double > f_array;
	int print_chem;
	int f_scalar_index;
	std::vector < int > time_step_scalar_indices;
	std::vector < std::string > time_steps;
	size_t time_step_max_len;
	std::vector < std::string > vector_names;
	size_t vector_name_max_len;
	size_t intermediate_idx;
	std::string hdf_prefix;
	std::string hdf_file_name;
	int scalar_count;			/* chemistry scalar count (doesn't include fortran scalars) */
	std::vector<std::string> g_hdf_scalar_names;
};
static std::vector < root_info > root;

/*
 *   statics used by all processes (including process 0)
 */
static struct proc_info
{
	std::vector<double> array;
} proc;


/* string constants */
static const char szTimeSteps[] = "TimeSteps";
static const char szHDF5Ext[] = ".h5";
static const char szX[] = "X";
static const char szY[] = "Y";
static const char szZ[] = "Z";
static const char szActive[] = "Active";
static const char szGrid[] = "Grid";
static const char szFeatures[] = "Features";
static const char szTimeStepFormat[] = "%.15g %s";
static const char szActiveArray[] = "ActiveArray";
static const char szScalars[] = "Scalars";
static const char szVectors[] = "Vectors";
static const char szVx_node[] = "Vx_node";
static const char szVy_node[] = "Vy_node";
static const char szVz_node[] = "Vz_node";
static const char szVmask[] = "Vmask";

/*
 *   Constants
 */
static const float INACTIVE_CELL_VALUE = 1.0e30f;

/*-------------------------------------------------------------------------
 * Function:         HDFBeginCTimeStep (called by all procs)
 *
 * Purpose:          TODO
 *
 * Preconditions:    TODO
 *
 * Postconditions:   TODO
 *-------------------------------------------------------------------------
 */
void
HDFBeginCTimeStep(int iso)
{
	if (root[iso].scalar_count == 0)
		return;;

	/* allocate space for this time step */
	assert(root[iso].active.size() > 0);
	assert(root[iso].scalar_count > 0);
	size_t array_count = root[iso].active.size() * root[iso].scalar_count;
	proc.array.resize(array_count);

	/* init entire array to inactive */
	for (size_t i = 0; i < array_count; ++i)
	{
		proc.array[i] = INACTIVE_CELL_VALUE;
	}
}

/*-------------------------------------------------------------------------
 * Function:         HDFEndCTimeStep   (called by all procs)
 *
 * Purpose:          Write chemistry scalars to HDF file
 *
 * Preconditions:    if (proc.cell_count > 0 && mpi_myself == 0)
 *                      root.current_file_dspace_id   OPENED (>0)
 *                      root.current_file_dset_id     OPENED (>0)
 *                   else
 *                      none
 *
 * Postconditions:   Chemistry scalars are written to HDF
 *-------------------------------------------------------------------------
 */
void
HDFEndCTimeStep(int iso)
{
	const int mpi_myself = 0;


	if (root[iso].active.size() == 0)
		return;					/* nothing to do */

		assert(root[iso].current_file_dspace_id > 0);	/* precondition */
		assert(root[iso].current_file_dset_id > 0);	/* precondition */

		/* write proc 0 data */
		write_proc_timestep(iso, mpi_myself, (int) root[iso].active.size(),
							root[iso].current_file_dspace_id,
							root[iso].current_file_dset_id, proc.array);
}

/*-------------------------------------------------------------------------
 * Function          FillHyperSlab
 *
 * Preconditions:    HDFBeginTimeStep has been called
 *
 * Postconditions:   TODO:
 *-------------------------------------------------------------------------
 */
void
HDFFillHyperSlab(int iso, std::vector< double > &d, size_t columns)
{
	// d is nxyz x columns, column dominant
	// writes data to proc.array, which is nactive x columns
	if (columns > 0)
	{
		assert (d.size()%columns == 0);
		for (size_t icol = 0; icol < columns; icol++)
		{
			for (int irow = 0; irow < root[iso].nxyz; irow++)
			{
				int iactive = root[iso].natural_to_active[irow];
				if (iactive >= 0)
				{
					proc.array[icol * root[iso].active.size() + iactive] = d[icol * root[iso].nxyz + irow];
				}
			}
		}
	}
}

/*-------------------------------------------------------------------------
 * Function          HDFFinalize (called by all procs)
 *
 * Preconditions:    TODO:
 *
 * Postconditions:   TODO:
 *-------------------------------------------------------------------------
 */
void
HDFFinalize(void)
{

	herr_t status;
	for (int iso = 0; iso < (int) root.size(); iso++)
	{
		if (root[iso].hdf_file_id == 0)
			return;

		assert(root[iso].current_file_dspace_id == -1);	/* shouldn't be open */
		assert(root[iso].current_file_dset_id == -1);	/* shouldn't be open */
		
		hdf_finalize_headings(iso);

		root[iso].scalar_names.clear();

		if (root[iso].vector_names.size() > 0)
		{
			/* free space */
			root[iso].vector_names.clear();
			root[iso].vector_name_max_len = 0;
		}

		if (root[iso].time_steps.size() > 0)
		{
			root[iso].time_steps.clear();
			root[iso].time_step_max_len = 0;
		}

		/* free mem */


		root[iso].f_array.clear();


		root[iso].natural_to_active.clear();


		root[iso].active.clear();

		/* close the file */
		assert(root[iso].hdf_file_id > 0);
		status = H5Fclose(root[iso].hdf_file_id);
		assert(status >= 0);


		/* set scalar count */
		root[iso].scalar_count = 0;
	}
}

/*-------------------------------------------------------------------------
 * Function          HDFInitialize (called by all procs)
 *
 * Preconditions:    HDF prefix
 *
 * Postconditions:   root                init for proc 0
 *                   proc                init for all procs
 *                   HDF file is opened as root.hdf_file_id
 *-------------------------------------------------------------------------
 */
void
HDFInitialize(int iso, const char *prefix, int prefix_l)
{
#if defined(NDEBUG)
#ifndef _WIN64
	H5Eset_auto(NULL, NULL);
#else
	H5Eset_auto1(NULL, NULL);
#endif
#endif
	if ((int) root.size() <= iso)
	{
		struct root_info tr;
		root.push_back(tr);
	}
	assert ((int) root.size() > iso);

	/* Open the HDF file */
	root[iso].hdf_file_id = open_hdf_file(iso, prefix, prefix_l);
	assert(root[iso].hdf_file_id > 0);

	root[iso].current_timestep_gr_id = -1;
	root[iso].current_file_dspace_id = -1;
	root[iso].current_file_dset_id = -1;
	root[iso].print_chem = -1;

	root[iso].time_step_scalar_indices.clear();
	root[iso].scalar_name_max_len = 0;

	root[iso].time_steps.clear();
	root[iso].time_step_max_len = 0;

	root[iso].vector_names.clear();
	root[iso].vector_name_max_len = 0;

	root[iso].intermediate_idx = 0;

	/* init proc */
	root[iso].scalar_count = 0;
}
void
HDFSetScalarNames(int iso, std::vector<std::string> &names)
{
		root[iso].g_hdf_scalar_names = names;
		root[iso].scalar_names = names;
		root[iso].scalar_count = (int) root[iso].scalar_names.size();
}

/*-------------------------------------------------------------------------
 * Function:         HDF_CLOSE_TIME_STEP (called only by proc 0)
 *
 * Purpose:          TODO
 *
 * Preconditions:    TODO
 *
 * Postconditions:   TODO
 *-------------------------------------------------------------------------
 */
void
HDF_CLOSE_TIME_STEP(int *iso_in)
{
	herr_t status;
	int iso = *iso_in;
		if (root[iso].current_file_dset_id > 0)
		{
			status = H5Dclose(root[iso].current_file_dset_id);
			assert(status >= 0);
		}
		root[iso].current_file_dset_id = -1;

		if (root[iso].current_file_dspace_id > 0)
		{
			status = H5Sclose(root[iso].current_file_dspace_id);
			assert(status >= 0);
		}
		root[iso].current_file_dspace_id = -1;

		if (root[iso].time_step_scalar_indices.size() > 0)
		{
			/* write the scalar indices for this timestep */
			hsize_t dims[1];
			hid_t dspace, dset;
			herr_t status;

			dims[0] = root[iso].time_step_scalar_indices.size();
			dspace = H5Screate_simple(1, dims, NULL);
			if (dspace <= 0)
			{
				assert(0);
				sprintf(error_string,
					"HDF ERROR: Unable to create file dataspace(DIM(%d)) for dataset /%s/%s\n",
					(int) dims[0], root[iso].timestep_buffer, szScalars);
				error_msg(error_string, STOP);
			}
			dset =
				H5Dcreate(root[iso].current_timestep_gr_id, szScalars, H5T_NATIVE_INT,
				dspace, H5P_DEFAULT);
			if (dset <= 0)
			{
				assert(0);
				sprintf(error_string,
					"HDF ERROR: Unable to create dataset /%s/%s\n",
					root[iso].timestep_buffer, szScalars);
				error_msg(error_string, STOP);
			}
			status =
				H5Dwrite(dset, H5T_NATIVE_INT, H5S_ALL, H5S_ALL, H5P_DEFAULT,
				&root[iso].time_step_scalar_indices.front());
			if (status < 0)
			{
				assert(0);
				sprintf(error_string,
					"HDF ERROR: Unable to write dataset /%s/%s\n",
					root[iso].timestep_buffer, szScalars);
				error_msg(error_string, STOP);
			}
			status = H5Dclose(dset);
			assert(status >= 0);

			root[iso].time_step_scalar_indices.clear();

			status = H5Sclose(dspace);
			assert(status >= 0);
		}

		/* close the time step group */
		if (root[iso].current_timestep_gr_id > 0)
		{
			status = H5Gclose(root[iso].current_timestep_gr_id);
			assert(status >= 0);
		}
		root[iso].current_timestep_gr_id = -1;

}

/*-------------------------------------------------------------------------
 * Function          hdf_finalize_headings
 *
 * Preconditions:    TODO:
 *
 * Postconditions:   TODO:
 *-------------------------------------------------------------------------
 */
static void
hdf_finalize_headings(int iso)
{
	int i;

	herr_t status;
	hid_t fls_type;

	assert(root[iso].current_file_dspace_id == -1);	// shouldn't be open
	assert(root[iso].current_file_dset_id == -1);	// shouldn't be open

	// create fixed length string type for /Scalar /TimeSteps and /Vectors
	fls_type = H5Tcopy(H5T_C_S1);
	if (fls_type <= 0)
	{
		assert(0);
		sprintf(error_string, "HDF ERROR: Unable to copy H5T_C_S1.\n");
		error_msg(error_string, STOP);
	}
	status = H5Tset_strpad(fls_type, H5T_STR_NULLTERM);
	if (status < 0)
	{
		assert(0);
		sprintf(error_string,
			"HDF ERROR: Unable to set size of fixed length string type(size=%d), 0.\n",
			(int) root[iso].scalar_name_max_len);
		error_msg(error_string, STOP);
	}

	if (root[iso].scalar_names.size() > 0)
	{
		hsize_t dims[1];
		hid_t dspace;
		hid_t dset;
		char *scalar_names;

		// write scalar names to file

		for (size_t j = 0; j < root[iso].scalar_names.size(); j++)
		{
			if (root[iso].scalar_names[j].size() + 1 > root[iso].scalar_name_max_len) 
				root[iso].scalar_name_max_len = root[iso].scalar_names[j].size() + 1;
		}

		status = H5Tset_size(fls_type, root[iso].scalar_name_max_len);
		if (status < 0)
		{
			assert(0);
			sprintf(error_string,
				"HDF ERROR: Unable to set size of fixed length string type(size=%d), 1.\n",
				(int) root[iso].scalar_name_max_len);
			error_msg(error_string, STOP);
		}

		assert (root[iso].scalar_names.size() > 0);

		// create the /Scalars dataspace
		dims[0] = root[iso].scalar_names.size();
		dspace = H5Screate_simple(1, dims, NULL);
		if (dspace <= 0)
		{
			assert(0);
			sprintf(error_string,
				"HDF ERROR: Unable to create the /%s dataset dataspace.\n",
				szScalars);
			error_msg(error_string, STOP);
		}

		// create the /Scalars dataset
		dset =
			H5Dcreate(root[iso].hdf_file_id, szScalars, fls_type, dspace,
			H5P_DEFAULT);
		if (dset <= 0)
		{
			assert(0);
			sprintf(error_string,
				"HDF ERROR: Unable to create the /%s dataset.\n",
				szScalars);
			error_msg(error_string, STOP);
		}

		// copy variable length scalar names to fixed length scalar names
		scalar_names =
			(char *) PHRQ_calloc(root[iso].scalar_name_max_len *
			root[iso].scalar_names.size(), sizeof(char));
		// java req'd
		for (i = 0; i < (int) root[iso].scalar_names.size(); ++i)
		{
			strcpy(scalar_names + i * root[iso].scalar_name_max_len,
				root[iso].scalar_names[i].c_str());
		}


		// write the /Scalars dataset
		status =
			H5Dwrite(dset, fls_type, H5S_ALL, H5S_ALL, H5P_DEFAULT,
			scalar_names);
		if (status < 0)
		{
			assert(0);
			sprintf(error_string,
				"HDF ERROR: Unable to write the /%s dataset.\n",
				szScalars);
			error_msg(error_string, STOP);
		}

		PHRQ_free(scalar_names);

		status = H5Sclose(dspace);
		assert(status >= 0);

		status = H5Dclose(dset);
		assert(status >= 0);
	}

	if (root[iso].vector_names.size() > 0)
	{
		hsize_t dims[1];
		hid_t dspace;
		hid_t dset;
		char *vector_names;

		// write vector names to file

		assert(root[iso].vector_names.size() == 1);	// Has a new vector been added?
		assert(root[iso].vector_names.size() > 0);

		status = H5Tset_size(fls_type, root[iso].vector_name_max_len);
		if (status < 0)
		{
			assert(0);
			sprintf(error_string,
				"HDF ERROR: Unable to set size of fixed length string type(size=%d), 2.\n",
				(int) root[iso].scalar_name_max_len);
			error_msg(error_string, STOP);
		}


		// create the /Vectors dataspace
		dims[0] = root[iso].vector_names.size();
		dspace = H5Screate_simple(1, dims, NULL);
		if (dspace <= 0)
		{
			assert(0);
			sprintf(error_string,
				"HDF ERROR: Unable to create the /%s dataset dataspace.\n",
				szVectors);
			error_msg(error_string, STOP);
		}

		// create the /Vectors dataset
		dset =
			H5Dcreate(root[iso].hdf_file_id, szVectors, fls_type, dspace,
			H5P_DEFAULT);
		if (dset <= 0)
		{
			assert(0);
			sprintf(error_string,
				"HDF ERROR: Unable to create the /%s dataset.\n",
				szVectors);
			error_msg(error_string, STOP);
		}

		// copy variable length vectors to fixed length strings
		vector_names =
			(char *) PHRQ_calloc(root[iso].vector_name_max_len *
			root[iso].vector_names.size(), sizeof(char));
		for (i = 0; i < (int) root[iso].vector_names.size(); ++i)
		{
			strcpy(vector_names + i * root[iso].vector_name_max_len,
				root[iso].vector_names[i].c_str());
		}

		// write the /Vectors dataset
		status =
			H5Dwrite(dset, fls_type, H5S_ALL, H5S_ALL, H5P_DEFAULT,
			vector_names);
		if (status < 0)
		{
			assert(0);
			sprintf(error_string,
				"HDF ERROR: Unable to write the /%s dataset.\n",
				szVectors);
			error_msg(error_string, STOP);
		}

		PHRQ_free(vector_names);

		status = H5Sclose(dspace);
		assert(status >= 0);

		status = H5Dclose(dset);
		assert(status >= 0);
	}

	if (root[iso].time_steps.size() > 0)
	{
		hsize_t dims[1];
		hid_t dspace;
		hid_t dset;
		char *time_steps;

		// write time step names to file

		status = H5Tset_size(fls_type, root[iso].time_step_max_len);
		if (status < 0)
		{
			assert(0);
			sprintf(error_string,
				"HDF ERROR: Unable to set size of fixed length string type(size=%d), 3.\n",
				(int) root[iso].time_step_max_len);
			error_msg(error_string, STOP);
		}

		assert(root[iso].time_steps.size() > 0);

		// create the /TimeSteps (szTimeSteps) dataspace
		dims[0] = root[iso].time_steps.size();
		dspace = H5Screate_simple(1, dims, NULL);
		if (dspace <= 0)
		{
			assert(0);
			sprintf(error_string,
				"HDF ERROR: Unable to create the /%s dataset dataspace.\n",
				szTimeSteps);
			error_msg(error_string, STOP);
		}

		// create the /TimeSteps (szTimeSteps) dataset
		dset =
			H5Dcreate(root[iso].hdf_file_id, szTimeSteps, fls_type, dspace,
			H5P_DEFAULT);
		if (dset <= 0)
		{
			assert(0);
			sprintf(error_string,
				"HDF ERROR: Unable to create the /%s dataset.\n",
				szTimeSteps);
			error_msg(error_string, STOP);
		}
		
		// copy variable length time steps to fixed length strings
		time_steps =
			(char *) PHRQ_calloc(root[iso].time_step_max_len *
			root[iso].time_steps.size(), sizeof(char));
		for (i = 0; i < (int) root[iso].time_steps.size(); ++i)
		{
			strcpy(time_steps + i * root[iso].time_step_max_len,
				root[iso].time_steps[i].c_str());
		}

		// write the /TimeSteps (szTimeSteps) dataset
		status =
			H5Dwrite(dset, fls_type, H5S_ALL, H5S_ALL, H5P_DEFAULT,
			time_steps);
		if (status < 0)
		{
			assert(0);
			sprintf(error_string,
				"HDF ERROR: Unable to write the /%s dataset.\n",
				szTimeSteps);
			error_msg(error_string, STOP);
		}

		PHRQ_free(time_steps);

		status = H5Sclose(dspace);
		assert(status >= 0);

		status = H5Dclose(dset);
		assert(status >= 0);
	}

	// close the fixed lenght string type
	status = H5Tclose(fls_type);
	assert(status >= 0);

	// close the file
	assert(root[iso].hdf_file_id > 0);
	status = H5Fclose(root[iso].hdf_file_id);
	assert(status >= 0);


	root[iso].hdf_file_id = H5Fopen(root[iso].hdf_file_name.c_str(), H5F_ACC_RDWR , H5P_DEFAULT);
	if (root[iso].hdf_file_id <= 0)
	{
		sprintf(error_string, "Unable to open HDF file:%s\n", root[iso].hdf_file_name.c_str());
		error_msg(error_string, STOP);
	}
#if H5_VERSION_GE(1, 10, 2)
	// force hdf5 1.8
	if (H5Fset_libver_bounds(root[iso].hdf_file_id, H5F_LIBVER_EARLIEST, H5F_LIBVER_V18) < 0)
	{
		sprintf(error_string, "H5Fset_libver_bounds failed:%s\n", root[iso].hdf_file_name.c_str());
		error_msg(error_string, STOP);
	}
#endif
}

/*-------------------------------------------------------------------------
 * Function          HDF_FINALIZE_INVARIANT (called only by proc 0)
 *
 * Preconditions:    root.grid_gr_id      open
 *                   root.features_gr_id  open
 *
 * Postconditions:   root.grid_gr_id      closed
 *                   root.features_gr_id  closed
 *-------------------------------------------------------------------------
 */
void
HDF_FINALIZE_INVARIANT(int *iso_in)
{
	herr_t status;
	int iso = *iso_in;

	status = H5Gclose(root[iso].grid_gr_id);
	assert(status >= 0);
	status = H5Gclose(root[iso].features_gr_id);
	assert(status >= 0);
}

/*-------------------------------------------------------------------------
 * Function          HDF_INITIALIZE_INVARIANT (called only by proc 0)
 *
 * Preconditions:    root.hdf_file_id    open
 *
 * Postconditions:   root.grid_gr_id       open
 *                   root.features_gr_id   open
 *-------------------------------------------------------------------------
 */
void
HDF_INITIALIZE_INVARIANT(int * iso_in)
{
	int iso = *iso_in;
	/*
	* Create the "/Grid" group
	*/
	assert(root[iso].hdf_file_id > 0);	/* precondition */
	root[iso].grid_gr_id = H5Gcreate(root[iso].hdf_file_id, szGrid, 0);
	if (root[iso].grid_gr_id <= 0)
	{
		sprintf(error_string, "Unable to create /%s group\n", szGrid);
		error_msg(error_string, STOP);
	}

	/*
	* Create the "/szFeatures" group
	*/
	root[iso].features_gr_id = H5Gcreate(root[iso].hdf_file_id, szFeatures, 0);
	if (root[iso].grid_gr_id <= 0)
	{
		sprintf(error_string, "Unable to create /%s group\n", szFeatures);
		error_msg(error_string, STOP);
	}
}

void
HDF_INTERMEDIATE(void)
{

	herr_t status;

	for (int iso = 0; iso < (int) root.size(); iso++)
	{
		// close the file
		assert(root[iso].hdf_file_id > 0);
		status = H5Fclose(root[iso].hdf_file_id);
		assert(status >= 0);

		// create intermediate filename
		char int_fn[MAX_PATH];
		sprintf(int_fn, "%s.intermediate%s", root[iso].hdf_prefix.c_str(), szHDF5Ext);

		// copy to the intermediate file
		char command[3*MAX_PATH];
#if WIN32
		sprintf(command, "copy \"%s\" \"%s\"", root[iso].hdf_file_name.c_str(), int_fn);
#else
		sprintf(command, "cp \"%s\" \"%s\"", root[iso].hdf_file_name.c_str(), int_fn);
#endif
		system(command);

		// open intermediate file for finalization
		root[iso].hdf_file_id = H5Fopen(int_fn, H5F_ACC_RDWR , H5P_DEFAULT);
		if (root[iso].hdf_file_id <= 0)
		{
			sprintf(error_string, "Unable to open HDF file:%s\n", int_fn);
			error_msg(error_string, STOP);
		}
#if H5_VERSION_GE(1, 10, 2)
		// force hdf5 1.8
		if (H5Fset_libver_bounds(root[iso].hdf_file_id, H5F_LIBVER_EARLIEST, H5F_LIBVER_V18) < 0)
		{
			sprintf(error_string, "H5Fset_libver_bounds failed:%s\n", int_fn);
			error_msg(error_string, STOP);
		}
#endif

		// finalize intermediate
		hdf_finalize_headings(iso);

		// close the file
		assert(root[iso].hdf_file_id > 0);
		status = H5Fclose(root[iso].hdf_file_id);
		assert(status >= 0);


		// reopen hdf file
		root[iso].hdf_file_id = H5Fopen(root[iso].hdf_file_name.c_str(), H5F_ACC_RDWR , H5P_DEFAULT);
		if (root[iso].hdf_file_id <= 0)
		{
			sprintf(error_string, "Unable to open HDF file:%s\n", root[iso].hdf_file_name.c_str());
			error_msg(error_string, STOP);
		}
#if H5_VERSION_GE(1, 10, 2)
		// force hdf5 1.8
		if (H5Fset_libver_bounds(root[iso].hdf_file_id, H5F_LIBVER_EARLIEST, H5F_LIBVER_V18) < 0)
		{
			sprintf(error_string, "H5Fset_libver_bounds failed:%s\n", root[iso].hdf_file_name.c_str());
			error_msg(error_string, STOP);
		}
#endif
	}
}

/*-------------------------------------------------------------------------
 * Function:         HDF_OPEN_TIME_STEP (called only by proc 0)
 *
 * Purpose:          TODO
 *
 * Preconditions:    TODO
 *
 * Postconditions:   TODO
 *-------------------------------------------------------------------------
 */
void
HDF_OPEN_TIME_STEP(int *iso_in, double *time, double *cnvtmi, int *print_chem,
				   int *print_vel, int *f_scalar_count)
{
	hsize_t dims[1];
	int i;
	size_t len;
	int iso = *iso_in;

	assert(root[iso].current_timestep_gr_id == -1);	/* shouldn't be open yet */
	assert(root[iso].current_file_dset_id == -1);	/* shouldn't be open yet */
	assert(root[iso].current_file_dspace_id == -1);	/* shouldn't be open yet */

	/* determine scalar count for this timestep */
	root[iso].print_chem = (*print_chem);
	int time_step_scalar_count =
		(root[iso].print_chem ? root[iso].scalar_count : 0) + (*f_scalar_count);
	if (time_step_scalar_count == 0 && *print_vel == 0)
	{
		return;					/* no hdf scalar or vector output for this time step */
	}

	/* format timestep string */
	sprintf(root[iso].timestep_buffer, szTimeStepFormat, (*time) * (*cnvtmi),
		root[iso].timestep_units);

	/* add time step string to list */;
	len = strlen(root[iso].timestep_buffer) + 1;
	if (root[iso].time_step_max_len < len)
		root[iso].time_step_max_len = len;;
	root[iso].time_steps.push_back(root[iso].timestep_buffer);

	/* Create the /<timestep string> group */
	assert(root[iso].timestep_buffer && strlen(root[iso].timestep_buffer));
	root[iso].current_timestep_gr_id =
		H5Gcreate(root[iso].hdf_file_id, root[iso].timestep_buffer, 0);
	if (root[iso].current_timestep_gr_id < 0)
	{
		assert(0);
		sprintf(error_string, "HDF ERROR: Unable to create group /%s\n",
			root[iso].timestep_buffer);
		error_msg(error_string, STOP);
	}

	if (time_step_scalar_count != 0)
	{

		/* allocate space for time step scalar indices */
		assert(root[iso].time_step_scalar_indices.size() == 0);
		root[iso].time_step_scalar_indices.resize(time_step_scalar_count);

		/* add cscalar indices (fortran indices are added one by one in PRNARR_HDF) */
		if (root[iso].print_chem)
		{
			for (i = 0; i < root[iso].scalar_count; ++i)
			{
				root[iso].time_step_scalar_indices[i] = i;
			}
		}

		/* Create the "/<timestep string>/ActiveArray" file dataspace. */
		dims[0] = root[iso].active.size() * root[iso].time_step_scalar_indices.size();
		root[iso].current_file_dspace_id = H5Screate_simple(1, dims, NULL);
		if (root[iso].current_file_dspace_id < 0)
		{
			assert(0);
			sprintf(error_string,
				"HDF ERROR: Unable to create dataspace(DIM=%d) for /%s/%s\n",
				(int) dims[0], root[iso].timestep_buffer, szActiveArray);
			error_msg(error_string, STOP);
		}

#ifdef SKIP_COMPRESSION			
		hid_t    plist_id; 
		hsize_t  cdims[2];
		plist_id  = H5Pcreate (H5P_DATASET_CREATE);

		/* Dataset must be chunked for compression */
		cdims[0] = 20;
		cdims[1] = 20;
		status = H5Pset_chunk (plist_id, 2, cdims);

		/* Set ZLIB / DEFLATE Compression using compression level 6.
		* To use SZIP Compression comment out these lines. 
		*/ 
		status = H5Pset_deflate (plist_id, 6); 

		/* Uncomment these lines to set SZIP Compression 
		szip_options_mask = H5_SZIP_NN_OPTION_MASK;
		szip_pixels_per_block = 16;
		status = H5Pset_szip (plist_id, szip_options_mask, szip_pixels_per_block);
		*/

		dataset_id = H5Dcreate2 (file_id, "Compressed_Data", H5T_STD_I32BE, 
			dataspace_id, H5P_DEFAULT, plist_id, H5P_DEFAULT); 
#endif




		/* Create the "/<timestep string>/ActiveArray" dataset */
		root[iso].current_file_dset_id =
			H5Dcreate(root[iso].current_timestep_gr_id, szActiveArray,
			H5T_NATIVE_FLOAT, root[iso].current_file_dspace_id,
			H5P_DEFAULT);
		if (root[iso].current_file_dset_id < 0)
		{
			assert(0);
			sprintf(error_string,
				"HDF ERROR: Unable to create dataset /%s/%s\n",
				root[iso].timestep_buffer, szActiveArray);
			error_msg(error_string, STOP);
		}
	}

	/* reset fortran scalar index */
	root[iso].f_scalar_index = 0;
}

/*-------------------------------------------------------------------------
 * Function:         HDF_PRNTAR
 *
 * Purpose:          TODO:
 *
 * Preconditions:    TODO:
 *
 * Postconditions:   TODO:
 *-------------------------------------------------------------------------
 */
void
HDF_PRNTAR(int *iso_in, double array[], double frac[], double *cnv, char *name)
{
	char name_buffer[120];
	hssize_t start[1];
	hsize_t dims[1], count[1];
	hid_t mem_dspace;
	herr_t status;
	int i;
	int iso = *iso_in;
	assert(root[iso].time_step_scalar_indices.size() > 0);

	/* copy and trim scalar name label */
	strcpy(name_buffer, name);
	//name_buffer[name_l] = '\0';
	//string_trim(name_buffer);

	/* check if this f_scalar has been added to root.scalar_names yet */
	/* phreeqc scalar count is proc.scalar_count */
	for (i = root[iso].scalar_count; i < (int) root[iso].scalar_names.size(); ++i)
	{
		if (root[iso].scalar_names[i] == name_buffer)
			break;
	}
	if (i == (int) root[iso].scalar_names.size())
	{
		size_t len = strlen(name_buffer) + 1;
		if (root[iso].scalar_name_max_len < len)
			root[iso].scalar_name_max_len = len;
		root[iso].scalar_names.push_back(name_buffer);
	}

	/* add this scalar index to the list of scalar indices */
	assert(((root[iso].print_chem ? root[iso].scalar_count : 0) + root[iso].f_scalar_index) <
		root[iso].time_step_scalar_indices.size());
	root[iso].time_step_scalar_indices[(root[iso].print_chem ? root[iso].scalar_count : 0) +
		root[iso].f_scalar_index] = i;

	/* copy the fortran scalar array into the active scalar array (f_array) */
	assert(root[iso].f_array.size() > 0);
	assert(root[iso].active.size() > 0);
	if ((root[iso].active.size() > 0) && (root[iso].f_array.size() > 0) && frac)
	{
		for (i = 0; i < (int) root[iso].active.size(); ++i)
		{
			assert(root[iso].active[i] >= 0 && root[iso].active[i] < root[iso].nxyz);
			if (frac[root[iso].active[i]] <= 0.0001)
			{
				root[iso].f_array[i] = INACTIVE_CELL_VALUE;
			}
			else
			{
				root[iso].f_array[i] = array[root[iso].active[i]] * (*cnv);
			}
		}
	}

	/* create the memory dataspace */
	dims[0] = root[iso].active.size();
	mem_dspace = H5Screate_simple(1, dims, NULL);
	if (mem_dspace <= 0)
	{
		assert(0);
		sprintf(error_string,
			"HDF Error: Unable to create memory dataspace\n");
		error_msg(error_string, STOP);
	}

	/* select within the file dataspace the hyperslab to write to */

	start[0] =
		(root[iso].f_scalar_index +
		(root[iso].print_chem ? root[iso].scalar_count : 0)) * root[iso].active.size();
	count[0] = root[iso].active.size();

	assert(root[iso].current_file_dspace_id > 0);	/* precondition */
	status =
		H5Sselect_hyperslab(root[iso].current_file_dspace_id, H5S_SELECT_SET,
		start, NULL, count, NULL);
	assert(status >= 0);

	/* Write the "/<timestep>/ActiveArray" dataset selection for this scalar */
	assert(root[iso].current_file_dset_id > 0);	/* precondition */
	if (H5Dwrite
		(root[iso].current_file_dset_id, H5T_NATIVE_DOUBLE, mem_dspace,
		root[iso].current_file_dspace_id, H5P_DEFAULT, &root[iso].f_array.front()) < 0)
	{
		assert(0);
		sprintf(error_string, "HDF Error: Unable to write dataset\n");
		error_msg(error_string, STOP);
	}

	/* Close the memory dataspace */
	status = H5Sclose(mem_dspace);
	assert(status >= 0);

	/* increment f_scalar_index */
	++root[iso].f_scalar_index;
}

/*-------------------------------------------------------------------------
 * Function:         HDF_VEL
 *
 * Purpose:          TODO:
 *
 * Preconditions:    TODO:
 *
 * Postconditions:   TODO:
 *-------------------------------------------------------------------------
 */
void
HDF_VEL(int *iso_in, double vx_node[], double vy_node[], double vz_node[], int vmask[])
{
	int i;
	const char name[] = "Velocities";
	int iso = *iso_in;

	/* check if the vector "Velocities" has been added to root.vector_names yet */
	for (i = 0; i < (int) root[iso].vector_names.size(); ++i)
	{
		if (strcmp(root[iso].vector_names[i].c_str(), name) == 0)
			break;
	}
	if (i == (int) root[iso].vector_names.size())
	{
		/* new scalar name */
		size_t len = strlen(name) + 1;
		if (root[iso].vector_name_max_len < len)
			root[iso].vector_name_max_len = len;
		root[iso].vector_names.push_back(name);
	}
	assert(root[iso].vector_names.size() == 1);	/* Has a new vector been added? */


	write_vector(iso, root[iso].current_timestep_gr_id, vx_node, root[iso].nxyz, szVx_node);
	write_vector(iso, root[iso].current_timestep_gr_id, vy_node, root[iso].nxyz, szVy_node);
	write_vector(iso, root[iso].current_timestep_gr_id, vz_node, root[iso].nxyz, szVz_node);
	write_vector_mask(iso, root[iso].current_timestep_gr_id, vmask, root[iso].nxyz, szVmask);
}

/*-------------------------------------------------------------------------
 * Function:         HDF_WRITE_FEATURE
 *
 * Purpose:          Write list of <feature_name> cell indices to HDF.
 *
 * Preconditions:    root.features_gr_id            created and open
 *
 * Postconditions:   <feature_name> dataset is written to HDF
 *-------------------------------------------------------------------------
 */
void
HDF_WRITE_FEATURE(int *iso_in, char *feature_name, int *nodes1, int *node_count)
{

	char feature_name_copy[120];
	int i;
	hsize_t dims[1], maxdims[1];
	hid_t dspace_id;
	hid_t dset_id;
	herr_t status;
	int *nodes0;
	int iso = *iso_in;

	if (*node_count == 0)
	{
		/* nothing to do */
		return;
	}

	/* copy and trim feature_name */
	strcpy(feature_name_copy, feature_name);
	//feature_name_copy[feature_name_l] = '\0';
	//string_trim(feature_name_copy);

	/* Create the "/szFeatures/feature_name" dataspace. */
	dims[0] = maxdims[0] = *node_count;
	dspace_id = H5Screate_simple(1, dims, maxdims);

	/* Create the "/szFeatures/feature_name" dataset */
	dset_id =
		H5Dcreate(root[iso].features_gr_id, feature_name_copy, H5T_NATIVE_INT,
		dspace_id, H5P_DEFAULT);

	/* Convert from 1-based to 0-based */
	nodes0 = (int *) PHRQ_malloc(sizeof(int) * (*node_count));
	if (nodes0 == NULL)
	{
		malloc_error();
		exit(4);
	}
	for (i = 0; i < *node_count; ++i)
	{
		nodes0[i] = nodes1[i] - 1;
	}

	/* Write the "/szFeatures/feature_name" dataset. */
	if (H5Dwrite
		(dset_id, H5T_NATIVE_INT, dspace_id, H5S_ALL, H5P_DEFAULT,
		nodes0) < 0)
	{
		printf("HDF Error: Unable to write \"/%s/%s\" dataset.\n", szFeatures,
			feature_name_copy);
		assert(0);
	}

	/* Close the "/szFeatures/feature_name" dataset */
	status = H5Dclose(dset_id);
	assert(status >= 0);

	/* Close the "/szFeatures/feature_name" dataspace. */
	status = H5Sclose(dspace_id);
	assert(status >= 0);

	PHRQ_free(nodes0);
}

/*-------------------------------------------------------------------------
 * Function:         HDF_WRITE_GRID (called by all procs)
 *
 * Purpose:          Writes x, y, z vectors and active cell list to HDF
 *                   file.
 *
 * Preconditions:    TODO
 *                   root.grid_gr_id            created and open
 *
 * Postconditions:   TODO
 *                   proc.scalar_count          set
 *                   root.nx, root.ny, root.nz  set
 *                   root.nxy, root.nxyz        set
 *-------------------------------------------------------------------------
 */
void
HDF_WRITE_GRID(int *iso_in, double x[], double y[], double z[],
			   int *nx, int *ny, int *nz,
			   int ibc[], char *UTULBL)
{
	int iso = *iso_in;
	/* copy and trim time units */
	strcpy(root[iso].timestep_units, UTULBL);
	//root[iso].timestep_units[UTULBL_l] = '\0';
	//string_trim(root[iso].timestep_units);

	assert(root[iso].grid_gr_id > 0);	/* precondition */

	write_axis(root[iso].grid_gr_id, x, *nx, szX);
	write_axis(root[iso].grid_gr_id, y, *ny, szY);
	write_axis(root[iso].grid_gr_id, z, *nz, szZ);

	root[iso].nx = *nx;
	root[iso].ny = *ny;
	root[iso].nz = *nz;
	root[iso].nxy = root[iso].nx * root[iso].ny;
	root[iso].nxyz = root[iso].nxy * root[iso].nz;

	assert(root[iso].active.size() == 0);
	assert(root[iso].natural_to_active.size() == 0);
	root[iso].natural_to_active.resize(root[iso].nxyz);
	int active_count = 0;
	for (int i = 0; i < root[iso].nxyz; ++i)
	{
		if (ibc[i] >= 0)
		{
			root[iso].natural_to_active[i] = active_count;
			root[iso].active.push_back(i);
			++active_count;
		}
		else
		{
			root[iso].natural_to_active[i] = -1;
		}
	}
	if (root[iso].active.size() <= 0)
	{
		error_msg("No active cells in model.", STOP);
	}

	/* allocate space for fortran scalars */
	assert(root[iso].f_array.size() == 0);
	root[iso].f_array.resize(root[iso].active.size());

	if ((int) root[iso].active.size() != root[iso].nxyz)
	{						/* Don't write if all are active */
		hsize_t dims[2], maxdims[2];
		hid_t dspace_id;
		hid_t dset_id;
		herr_t status;

		/* Create the "/Grid/Active" dataspace. */
		dims[0] = maxdims[0] = root[iso].active.size();
		dspace_id = H5Screate_simple(1, dims, maxdims);
		assert(dspace_id > 0);

		/* Create the "/Grid/Active" dataset */
		dset_id =
			H5Dcreate(root[iso].grid_gr_id, szActive, H5T_NATIVE_INT,
			dspace_id, H5P_DEFAULT);
		assert(dset_id > 0);

		/* Write the "/Grid/Active" dataset */
		if (H5Dwrite
			(dset_id, H5T_NATIVE_INT, dspace_id, H5S_ALL, H5P_DEFAULT,
			&root[iso].active.front()) < 0)
		{
			printf("HDF Error: Unable to write \"/%s/%s\" dataset\n",
				szGrid, szActive);
		}

		/* Close the "/Grid/Active" dataset */
		status = H5Dclose(dset_id);
		assert(status >= 0);

		/* Close the "/Grid/Active" dataspace */
		status = H5Sclose(dspace_id);
		assert(status >= 0);
	}

	root[iso].scalar_count = (int) root[iso].g_hdf_scalar_names.size();
}

/*-------------------------------------------------------------------------
 * Function          open_hdf_file
 *
 * Preconditions:    TODO:
 *
 * Postconditions:   TODO:
 *-------------------------------------------------------------------------
 */
static hid_t
open_hdf_file(int iso, const char *prefix, int prefix_l)
{

	hid_t file_id;
	char hdf_prefix[257];
	char hdf_file_name[257];
	char hdf_backup_name[257];

	strncpy(hdf_prefix, prefix, prefix_l);
	hdf_prefix[prefix_l] = '\0';
	string_trim(hdf_prefix);

	sprintf(hdf_file_name, "%s%s", hdf_prefix, szHDF5Ext);


	root[iso].hdf_prefix    = hdf_prefix;
	root[iso].hdf_file_name = hdf_file_name;
	if (file_exists(hdf_file_name))
	{
		sprintf(hdf_backup_name, "%s%s~", hdf_prefix, szHDF5Ext);
		if (file_exists(hdf_backup_name))
			remove(hdf_backup_name);
		rename(hdf_file_name, hdf_backup_name);
	}


	file_id =
		H5Fcreate(hdf_file_name, H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT);
	if (file_id <= 0)
	{
		sprintf(error_string, "Unable to open HDF file:%s\n", hdf_file_name);
		error_msg(error_string, STOP);
	}
#if H5_VERSION_GE(1, 10, 2)
	// force hdf5 1.8
	if (H5Fset_libver_bounds(file_id, H5F_LIBVER_EARLIEST, H5F_LIBVER_V18) < 0)
	{
		sprintf(error_string, "H5Fset_libver_bounds failed file:%s\n", hdf_file_name);
		error_msg(error_string, STOP);
	}
#endif
	return file_id;
}

/*-------------------------------------------------------------------------
 * Function:         write_axis
 *
 * Purpose:          Writes the given vector <name> to the loc_id group.
 *
 * Preconditions:    loc_id                     open
 *
 * Postconditions:   "loc_id/<name>" is written to HDF
 *-------------------------------------------------------------------------
 */
static void
write_axis(hid_t loc_id, double *a, int na, const char *name)
{
	hsize_t dims[1], maxdims[1];
	hid_t dspace_id;
	hid_t dset_id;
	herr_t status;

	if (!(na > 0))
		return;

	/* Create the "/Grid/name" dataspace. */
	dims[0] = maxdims[0] = na;
	dspace_id = H5Screate_simple(1, dims, maxdims);

	/* Create the "/Grid/name" dataset */
	dset_id =
		H5Dcreate(loc_id, name, H5T_NATIVE_FLOAT, dspace_id, H5P_DEFAULT);

	/* Write the "/Grid/name" dataset */
	if (H5Dwrite
		(dset_id, H5T_NATIVE_DOUBLE, dspace_id, H5S_ALL, H5P_DEFAULT, a) < 0)
	{
		sprintf(error_string,
				"HDF Error: Unable to write \"/%s/%s\" dataset\n", szGrid,
				name);
		error_msg(error_string, STOP);
	}

	/* Close the "/Grid/name" dataset */
	status = H5Dclose(dset_id);
	assert(status >= 0);

	/* Close the "/Grid/name" dataspace */
	status = H5Sclose(dspace_id);
	assert(status >= 0);
}

/*-------------------------------------------------------------------------
 * Function          write_proc_timestep
 *
 * Preconditions:    Called only by proc 0 (for each proc including 0)
 *
 * Postconditions:   Timestep for the process(rank) is written to HDF
 *-------------------------------------------------------------------------
 */
static void
write_proc_timestep(int iso, int rank, int cell_count, hid_t file_dspace_id,
					hid_t dset_id, std::vector<double> &array)
{

	hid_t mem_dspace;
	herr_t status;
	hsize_t dims[1];
	hsize_t count[1];
	hssize_t start[1];

	/* create the memory dataspace */
	dims[0] = root[iso].active.size() * root[iso].scalar_count;
	assert(dims[0] > 0);
	mem_dspace = H5Screate_simple(1, dims, NULL);
	if (mem_dspace < 0)
	{
		sprintf(error_string,
				"HDF ERROR: Unable to create memory dataspace for process %d\n",
				rank);
		error_msg(error_string, STOP);
	}

	start[0] = 0;
	count[0] = root[iso].active.size() * root[iso].scalar_count;
	status = H5Sselect_hyperslab(root[iso].current_file_dspace_id, H5S_SELECT_SET,
		start, NULL, count, NULL);
	assert(status >= 0);

	status = H5Dwrite(dset_id, H5T_NATIVE_DOUBLE, mem_dspace, file_dspace_id,
		H5P_DEFAULT, &array.front());
	if (status < 0)
	{
		sprintf(error_string, "HDF ERROR: Unable to write dataspace\n");
		error_msg(error_string, STOP);
	}

	status = H5Sclose(mem_dspace);
	assert(status >= 0);
}

/*-------------------------------------------------------------------------
 * Function:         write_vector
 *
 * Purpose:          Writes the given vector <name> to the loc_id group.
 *
 * Preconditions:    loc_id                     open
 *
 * Postconditions:   "loc_id/<name>" is written to HDF
 *-------------------------------------------------------------------------
 */
static void
write_vector(int iso, hid_t loc_id, double a[], int na, const char *name)
{
	hsize_t dims[1];
	hid_t dspace_id;
	hid_t dset_id;
	herr_t status;

	if (na <= 0)
		return;

	/* Create the "/<timestep string>/name" dataspace. */
	dims[0] = na;
	dspace_id = H5Screate_simple(1, dims, NULL);

	/* Create the "/<timestep string>/name" dataset */
	dset_id =
		H5Dcreate(loc_id, name, H5T_NATIVE_FLOAT, dspace_id, H5P_DEFAULT);

	/* Write the "/<timestep string>/name" dataset */
	if (H5Dwrite
		(dset_id, H5T_NATIVE_DOUBLE, dspace_id, H5S_ALL, H5P_DEFAULT, a) < 0)
	{
		sprintf(error_string,
				"HDF Error: Unable to write \"/%s/%s\" dataset\n",
				root[iso].timestep_buffer, name);
		error_msg(error_string, STOP);
	}

	/* Close the "/<timestep string>/name" dataset */
	status = H5Dclose(dset_id);
	assert(status >= 0);

	/* Close the "/<timestep string>/name" dataspace */
	status = H5Sclose(dspace_id);
	assert(status >= 0);
}

/*-------------------------------------------------------------------------
 * Function:         write_vector_mask
 *
 * Purpose:          Writes the given vector <name> to the loc_id group.
 *
 * Preconditions:    loc_id                     open
 *
 * Postconditions:   "loc_id/<name>" is written to HDF
 *-------------------------------------------------------------------------
 */
static void
write_vector_mask(int iso, hid_t loc_id, int a[], int na, const char *name)
{
	hsize_t dims[1];
	hid_t dspace_id;
	hid_t dset_id;
	herr_t status;

	if (na <= 0)
		return;

	/* Create the "/<timestep string>/name" dataspace. */
	dims[0] = na;
	dspace_id = H5Screate_simple(1, dims, NULL);

	/* Create the "/<timestep string>/name" dataset */
	dset_id = H5Dcreate(loc_id, name, H5T_NATIVE_INT, dspace_id, H5P_DEFAULT);

	/* Write the "/<timestep string>/name" dataset */
	if (H5Dwrite(dset_id, H5T_NATIVE_INT, dspace_id, H5S_ALL, H5P_DEFAULT, a)
		< 0)
	{
		sprintf(error_string,
				"HDF Error: Unable to write \"/%s/%s\" dataset\n",
				root[iso].timestep_buffer, name);
		error_msg(error_string, STOP);
	}

	/* Close the "/<timestep string>/name" dataset */
	status = H5Dclose(dset_id);
	assert(status >= 0);

	/* Close the "/<timestep string>/name" dataspace */
	status = H5Sclose(dspace_id);
	assert(status >= 0);
}

/* ---------------------------------------------------------------------- */
int 
string_trim(char *str)
/* ---------------------------------------------------------------------- */
{
/*
 *   Function trims white space from left and right of string
 *
 *   Arguments:
 *      str      string to trime
 *
 *   Returns
 *      TRUE     if string was changed
 *      FALSE    if string was not changed
 *      EMPTY    if string is all whitespace
 */
	int i, l, start, end, length;
	char *ptr_start;

	l = (int) strlen(str);
	/*
	 *   leading whitespace
	 */
	for (i = 0; i < l; i++)
	{
		if (isspace((int) str[i]))
			continue;
		break;
	}
	if (i == l)
		return (EMPTY);
	start = i;
	ptr_start = &(str[i]);
	/*
	 *   trailing whitespace
	 */
	for (i = l - 1; i >= 0; i--)
	{
		if (isspace((int) str[i]))
			continue;
		break;
	}
	end = i;
	if (start == 0 && end == l)
		return (FALSE);
	length = end - start + 1;
	memmove((void *) str, (void *) ptr_start, (size_t) length);
	str[length] = '\0';

	return (TRUE);
}
/* ---------------------------------------------------------------------- */
void 
malloc_error(void)
/* ---------------------------------------------------------------------- */
{
	//error_msg("NULL pointer returned from malloc or realloc.", CONTINUE);
	//error_msg("Program terminating.", STOP);
	std::cerr << "NULL pointer returned from malloc or realloc in hdf.cpp.\n";
	std::cerr << "Program terminating." << std::endl;
	exit(4);
}
/* ---------------------------------------------------------------------- */
void 
error_msg(const char * msg, int stop)
/* ---------------------------------------------------------------------- */
{

	std::cerr << msg << "\n";
	if (stop == STOP)
	{
		std::cerr << "Program terminating." << std::endl;
		exit(4);
	}
}
/*-------------------------------------------------------------------------
 * Function          file_exists
 *
 * Preconditions:    TODO:
 *
 * Postconditions:   TODO:
 *-------------------------------------------------------------------------
 */
int
file_exists(const char *name)
{
	FILE *stream;
	if ((stream = fopen(name, "r")) == NULL)
	{
		return 0;				/* doesn't exist */
	}
	fclose(stream);
	return 1;					/* exists */
}
