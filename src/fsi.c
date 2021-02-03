#include "fsi.h"
#include "SolverInterfaceC.h"
#include <float.h>
#include <math.h>
#include <string.h>
#include <stdio.h>

#ifdef BOOL_TRUE
#error BOOL_TRUE already defined!
#endif

#ifdef BOOL_FALSE
#error BOOL_TRUE already defined!
#endif

#define BOOL_TRUE  1
#define BOOL_FALSE 0

double timestep_limit = 0.0;
double* forces = NULL;
int* force_indices = NULL;
int skip_grid_motion = BOOL_TRUE;
int did_gather_write_positions = BOOL_FALSE;
int did_gather_read_positions = BOOL_FALSE;
int thread_index = 0;
int dynamic_thread_size = 0;
int wet_edges_size = 0;
int wet_nodes_size = 0;
int boundary_nodes_size = 0;
int deformable_nodes_size = 0;
int moved_nodes_counter = 0;
double* initial_coords = NULL;
double* boundary_coords = NULL; /* MESH MOVEMENT */
double* displacements = NULL;
int* displ_indices = NULL;
int* dynamic_thread_node_size = NULL;
double* c_matrix = NULL;
double* x_coeff_vector = NULL;
double* y_coeff_vector = NULL;
double* b_vector = NULL;
int* pivots_vector = NULL;
int comm_size = -1;
int require_create_checkpoint = BOOL_FALSE;
int* precice_force_ids; /* Gathered in host node (or serial node) */
int* precice_displ_ids;

#if ND_ND == 2
#define norm(a, b) sqrt((a[0]-b[0])*(a[0]-b[0]) + (a[1]-b[1])*(a[1]-b[1]))
#else
#error Not implemented!
#endif

/* Forward declarations of helper functions */
void count_dynamic_threads();
void write_forces();
void read_displacements(Dynamic_Thread* dt);
int check_write_positions();
int check_read_positions(Dynamic_Thread* dt);
void set_mesh_positions(Domain* domain);

/* This function creates the solver interface named "Fluent" and initializes
 * the interface
 * fsi_init is directly called by FLUENT UDF Functionality
 * */
void fsi_init(Domain* domain)
{
  int precice_process_id = -1; /* Process ID given to preCICE */
  printf("\nEntering fsi_init\n");

  /* Only Host Process (Rank 0) handles the coupling interface */
  #if !RP_HOST

  #if !PARALLEL
  precice_process_id = 0;
  comm_size = 1;
  #else /* !PARALLEL*/
  #if RP_HOST
  precice_process_id = 0;
  #elif RP_NODE
  precice_process_id = myid + 1;
  #endif /* elif RP_NODE */
  comm_size = compute_node_count + 1;
  #endif /* else !PARALLEL */

  /* Parallel implementation above is bypassed for testing serial version */
  precice_process_id = 0;
  comm_size = 1;

  Message("  (%d) Creating solver interface\n", myid);

  /* temporarily hard coding Solver name and preCICE Config File name  */
  precicec_createSolverInterface("Fluent", "precice-config.xml",
                                precice_process_id, comm_size);

  // count_dynamic_threads();

  /* Set coupling mesh */
  set_mesh_positions(domain);

  Message("  (%d) Initializing coupled simulation\n", myid);
  timestep_limit = precicec_initialize();
  Message("  (%d) Initialization done\n", myid);

  if (precicec_isActionRequired(precicec_actionWriteIterationCheckpoint())){
    Message("  (%d) Implicit coupling\n", myid);
    #if !RP_NODE
    RP_Set_Integer("udf/convergence", BOOL_FALSE);
    RP_Set_Integer("udf/iterate", BOOL_TRUE);
    #endif /* ! RP_NODE */
    precicec_markActionFulfilled(precicec_actionWriteIterationCheckpoint());
  }
  else {
    Message("  (%d) Explicit coupling\n", myid);
  }

  Message("  (%d) Synchronizing Fluent processes\n", myid);
  PRF_GSYNC();

  printf("(%d) Leaving INIT\n", myid);
  #endif /* !RP_HOST */
}

/* Main function advances the interface time step and provides the mechanism
 * for proper coupling scheme to be applied
 * fsi_write_and_advance is directly called by FLUENT UDF functionality
 * */
void fsi_write_and_advance()
{
  /* Only the host process (Rank 0) handles the writing of data and advancing coupling */
  #if !RP_HOST

  printf("(%d) Entering ON_DEMAND(write_and_advance)\n", myid);
  int ongoing;
  int subcycling = !precicec_isWriteDataRequired(CURRENT_TIMESTEP);
  int current_size = -1;

  if (subcycling){
    Message("  (%d) In subcycle, skip writing\n", myid);
  }
  else {
    if (wet_edges_size){
      write_forces();
    }
  }

  timestep_limit = precicec_advance(CURRENT_TIMESTEP);

  /* Read coupling state */
  ongoing = precicec_isCouplingOngoing();
  #if !RP_NODE
  RP_Set_Integer("udf/ongoing", ongoing);
  #endif /* !RP_NODE */

  if (precicec_isActionRequired(precicec_actionWriteIterationCheckpoint())){
    #if !RP_NODE
    RP_Set_Integer("udf/convergence", BOOL_TRUE);
    #endif /* !RP_NODE */
    precicec_markActionFulfilled(precicec_actionWriteIterationCheckpoint());
  }

  if (precicec_isActionRequired(precicec_actionReadIterationCheckpoint())){
    #if !RP_NODE
    RP_Set_Integer("udf/convergence", BOOL_FALSE);
    #endif /* !RP_NODE */
    precicec_markActionFulfilled(precicec_actionReadIterationCheckpoint());
  }

  #if !RP_NODE
  if (! precicec_isCouplingOngoing()){
    RP_Set_Integer("udf/convergence", BOOL_TRUE);
  }
  #endif /* !RP_NODE */

  printf("(%d) Leaving ON_DEMAND(write_and_advance)\n", myid);
  #endif /* !RP_HOST */
}

/* Function to be attached to the Dynamic Mesh in FLUENT in the form of a UDF.
 * This function will read the displacements values from interface and move the
 * structural mesh accordingly
 * fsi_grid_motion is directly related to mesh motion in FLUENT UDF Functionality
 * */
void fsi_grid_motion(Domain* domain, Dynamic_Thread* dt, real time, real dtime)
{
  /* Only the host process (Rank 0) handles grid motion and displacement calculations */
  #if !RP_HOST

  printf("\n(%d) Entering GRID_MOTION\n", myid);
  int current_thread_size = -1;

  if (thread_index == dynamic_thread_size){
    printf ("Reset thread index\n");
    thread_index = 0;
  }
  printf("  (%d) Thread index = %d\n", myid, thread_index);
  Thread* face_thread  = DT_THREAD(dt);

  if (strncmp("gridmotions", dt->profile_udf_name, 11) != 0){
    printf("  (%d) ERROR: called gridmotions for invalid dynamic thread: %s\n",
            myid, dt->profile_udf_name);
    exit(1);
  }
  if (face_thread == NULL){
    printf("  (%d) ERROR: face_thread == NULL\n", myid);
    exit(1);
  }

  if (skip_grid_motion){
    if (thread_index >= dynamic_thread_size-1){
      skip_grid_motion = BOOL_FALSE;
    }
    thread_index++;
    printf("  (%d) Skipping first round grid motion\n", myid);
    return;
  }

  SET_DEFORMING_THREAD_FLAG(THREAD_T0(face_thread));

  read_displacements(dt);
  thread_index++;

  #if !RP_NODE

  Message("  (%d) convergence=%d, iterate=%d, couplingOngoing=%d\n",
          myid, RP_Get_Integer("udf/convergence"), RP_Get_Integer("udf/iterate"),
          precicec_isCouplingOngoing());
  if (RP_Get_Integer("udf/convergence") && RP_Get_Integer("udf/iterate") && precicec_isCouplingOngoing()){
    RP_Set_Integer("udf/convergence", BOOL_FALSE);
  }

  #endif /* !RP_NODE */

  if (! precicec_isCouplingOngoing()){
    precicec_finalize();
  }

  printf("(%d) Leaving GRID_MOTION\n", myid);

  #endif /* !RP_HOST  */
}

void set_mesh_positions(Domain* domain)
{
  /* Only the host process (Rank 0) handles grid motion and displacement calculations */
  #if !RP_HOST

  printf("(%d) Entering set_mesh_positions()\n", myid);
  Thread* face_thread  = NULL;
  Dynamic_Thread* dynamic_thread = NULL;
  Node* node;
  face_t face;
  int n = 0, dim = 0, array_index = 0;
  int meshID = precicec_getMeshID("moving_base");

  if (domain->dynamic_threads == NULL){
    Message("  (%d) ERROR: domain.dynamic_threads == NULL\n", myid);
    exit(1);
  }
  dynamic_thread = domain->dynamic_threads;

  face_thread = DT_THREAD(dynamic_thread);
  if (face_thread == NULL){
	printf("  (%d) ERROR: face_thread == NULL\n", myid);
    fflush(stdout);
    exit(1);
  }

  /* Count number of interface vertices and dynamic_thread_node_size */
  begin_f_loop(face, face_thread){
    if (PRINCIPAL_FACE_P(face,face_thread)){
      f_node_loop(face, face_thread, n){
        node = F_NODE(face, face_thread, n);
        printf("Count of wet_nodes_size = %d\n", wet_nodes_size);
        wet_nodes_size++;
        dynamic_thread_node_size[thread_index]++;
      }
    }
  } end_f_loop(face, face_thread);

  printf("  (%d) Setting %d initial positions ...\n", myid, wet_nodes_size);

  /* Providing mesh information to preCICE */
  initial_coords = (double*) malloc(wet_nodes_size * ND_ND * sizeof(double));
  displacements = (double*) malloc(wet_nodes_size * ND_ND * sizeof(double));
  displ_indices = (int*) malloc(wet_nodes_size * sizeof(int));
  array_index = wet_nodes_size - dynamic_thread_node_size[thread_index];

  begin_f_loop (face, face_thread){
    if (PRINCIPAL_FACE_P(face,face_thread)){
      f_node_loop(face, face_thread, n){
		    node = F_NODE(face, face_thread, n);
        NODE_MARK(node) = 1;  /*Set node to need update*/
        for (dim = 0; dim < ND_ND; dim++){
           initial_coords[array_index*ND_ND+dim] = NODE_COORD(node)[dim];
        }
        array_index++;
      }
    }
  } end_f_loop(face, face_thread);

  for (int i=0; i<wet_nodes_size; i++) {
    printf("initial_coords[%d] = (%f, %f)\n", i, initial_coords[i],
            initial_coords[i+1]);
  }

  precicec_setMeshVertices(meshID, wet_nodes_size, initial_coords, displ_indices);

  printf("  (%d) Set %d (of %d) mesh positions ...\n", myid,
          array_index - wet_nodes_size + dynamic_thread_node_size[thread_index],
          dynamic_thread_node_size[thread_index]);

  printf("(%d) Leaving set_mesh_positions()\n", myid);

  #endif /* !RP_HOST  */
}

/* This functions reads the new displacements provided by the structural
 * solver and moves the mesh coordinates in FLUENT with the corresponding
 * values
 * */
void read_displacements(Dynamic_Thread* dt)
{
  int meshID = precicec_getMeshID("moving_base");
  int displID = precicec_getDataID("Displacements", meshID);
  int offset = 0;
  int i = 0, n = 0, dim = 0;
  Thread* face_thread  = DT_THREAD(dt);
  Node* node;
  face_t face;
  real max_displ_delta = 0.0;

  if (dynamic_thread_node_size[thread_index] > 0){
    Message("  (%d) Reading displacements...\n", myid);
    offset = 0;
    for (i = 0; i < thread_index; i++){
      offset += dynamic_thread_node_size[i];
    }
    printf("data size for readBlockVectorData = %d\n",dynamic_thread_node_size[thread_index]);
  precicec_readBlockVectorData(displID, dynamic_thread_node_size[thread_index],
        displ_indices + offset, displacements + ND_ND * offset);
	printf("After readBlockVectorData\n");

  Message("  (%d) Setting displacements...\n", myid);
  i = offset * ND_ND;
  begin_f_loop (face, face_thread){
    if (PRINCIPAL_FACE_P(face,face_thread)){
      f_node_loop (face, face_thread, n){
        node = F_NODE(face, face_thread, n);
        if (NODE_POS_NEED_UPDATE(node)){
          NODE_POS_UPDATED(node);
          for (dim=0; dim < ND_ND; dim++){
            /* NODE_COORD(node)[dim] = initial_coords[i+dim] + displacements[i+dim]; */
            if (fabs(displacements[i+dim]) > fabs(max_displ_delta)){
              max_displ_delta = displacements[i + dim];
            }
          }
          i += ND_ND;
        }
      }
    }
  } end_f_loop (face, face_thread);

  Message("  (%d) ...done\n", myid);
  }
  Message("  (%d) Max displacement delta: %f\n", myid, max_displ_delta);
}

/* This function writes the new forces on the structure calculated in FLUENT to the
 * Structural solver
 */
void write_forces()
{
  int meshID = precicec_getMeshID("moving_base");
  int forceID = precicec_getDataID("Forces", meshID);
  int i=0, j=0;
  Domain* domain = NULL;
  Dynamic_Thread* dynamic_thread = NULL;
  int thread_counter = 0;
  real area[ND_ND];
  real pressure_force[ND_ND];
  real viscous_force[ND_ND];
  double total_force[ND_ND];
  double max_force = 0.0;

  domain = Get_Domain(1);
  if (domain == NULL){
    Message("  (%d) ERROR: domain == NULL\n", myid);
    exit(1);
  }
  if (domain->dynamic_threads == NULL){
    Message("  (%d) ERROR: domain.dynamic_threads == NULL\n", myid);
    exit(1);
  }

  dynamic_thread = domain->dynamic_threads;
  thread_counter = 0;
  Message("  (%d) Gather forces...\n", myid);
  i = 0;
  while (dynamic_thread != NULL){
    if (strncmp("gridmotions", dynamic_thread->profile_udf_name, 11) == 0){
      Message("  (%d) Thread index %d\n", myid, thread_counter);
      Thread* face_thread  = DT_THREAD(dynamic_thread);
      if (face_thread == NULL){
        Message("  (%d) ERROR: face_thread == NULL\n", myid);
        exit(1);
      }
      face_t face;
      begin_f_loop (face, face_thread){
        if (PRINCIPAL_FACE_P(face,face_thread)){
          F_AREA(area, face, face_thread);
          NV_VS(viscous_force, =, F_STORAGE_R_N3V(face,face_thread,SV_WALL_SHEAR),*,-1.0);
          NV_VS(pressure_force, =, area, *, F_P(face,face_thread));
          NV_VV(total_force, =, viscous_force, +, pressure_force);
          for (j=0; j < ND_ND; j++){
            forces[i + j] = total_force[j];
            if (fabs(total_force[j]) > fabs(max_force)){
              max_force = total_force[j];
            }
          }
          i += ND_ND;
        }
      } end_f_loop(face, face_thread);
      thread_counter++;
    }
    dynamic_thread = dynamic_thread->next;
  }
  Message("  (%d) ...done (with %d force values)\n", myid, i);
  Message("  (%d) Writing forces...\n", myid);
  precicec_writeBlockVectorData(forceID, wet_edges_size, force_indices, forces);
  Message("  (%d) ...done\n", myid );
  Message("  (%d) Max force: %f\n", max_force);
  if (thread_counter != dynamic_thread_size){
    Message ( "  (%d) ERROR: Number of dynamic threads has changed to %d!\n", myid, thread_counter );
    exit(1);
  }
}

int check_write_positions()
{
  #if !RP_HOST
  Domain* domain = NULL;
  Dynamic_Thread* dynamic_thread = NULL;
  Thread* face_thread = NULL;
  int thread_counter = 0;
  face_t face;
  int wet_edges_check_size = 0;

  Message("  (%d) Checking write positions...\n", myid);
  domain = Get_Domain(1);
  if (domain == NULL){
    Message("  (%d) ERROR: domain == NULL\n", myid);
    exit(1);
  }
  if (domain->dynamic_threads == NULL){
    Message("  (%d) ERROR: domain.dynamic_threads == NULL\n", myid);
    exit(1);
  }
  dynamic_thread = domain->dynamic_threads;
  thread_counter = 0;
  while (dynamic_thread != NULL){
    if (strncmp("gridmotions", dynamic_thread->profile_udf_name, 11) == 0){
      /*Message("\n  (%d) Thread index %d\n", myid, thread_counter);*/
      face_thread  = DT_THREAD(dynamic_thread);
      if (face_thread == NULL){
        Message("  (%d) ERROR: Thread %d: face_thread == NULL\n", myid, thread_counter);
        exit(1);
      }
      begin_f_loop (face, face_thread){
        if (PRINCIPAL_FACE_P(face,face_thread)){
          wet_edges_check_size++;
        }
      } end_f_loop(face, face_thread);
      thread_counter++;
    }
    dynamic_thread = dynamic_thread->next;
  }
  Message("  (%d) ...done (currently %d wet edges, old is %d)", myid,
          wet_edges_check_size, wet_edges_size);
  if (wet_edges_check_size != wet_edges_size) {
    return wet_edges_check_size;
  }
  #endif /* ! RP_HOST */
  return -1;
}

int check_read_positions(Dynamic_Thread* dt)
{
  Message("  (%d) Checking read positions...\n", myid);
  int i = 0, n = 0;
  Thread* face_thread = DT_THREAD(dt);
  Node* node;
  face_t face;

  /* Count nodes */
  begin_f_loop(face, face_thread){
    if (PRINCIPAL_FACE_P(face,face_thread)){
      f_node_loop (face, face_thread, n){
        node = F_NODE(face, face_thread, n);
        if (NODE_POS_NEED_UPDATE(node)){
          NODE_MARK(node) = 12345;
          i++;
        }
      }
    }
  } end_f_loop(face, face_thread);

  /* Reset node marks */
  begin_f_loop(face, face_thread){
    if (PRINCIPAL_FACE_P(face,face_thread)){
      f_node_loop (face, face_thread, n){
        node = F_NODE(face, face_thread, n);
        if (NODE_MARK(node) == 12345){
          NODE_MARK(node) = 1; /* Set node to need update*/
        }
      }
    }
  } end_f_loop(face, face_thread);

  if (i != dynamic_thread_node_size[thread_index]){
    Message("  (%d) Wet node count has changed for dynamic thread %d!\n",
            myid, thread_index);
    return i;
  }
  return -1;
}
