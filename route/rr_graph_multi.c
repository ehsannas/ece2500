#include <stdio.h>
#include <math.h>
#include <assert.h>
#include <string.h>
#include <set>
#include "util.h"
#include "vpr_types.h"
#include "globals.h"
#include "rr_graph_util.h"
#include "rr_graph.h"
#include "rr_graph2.h"
#include "rr_graph_multi.h"
#include "rr_graph_sbox.h"
#include "check_rr_graph.h"
#include "rr_graph_timing_params.h"
#include "rr_graph_indexed_data.h"
#include "vpr_utils.h"

//#define DUMP_DEBUG_FILES
#define USE_NODE_DUPLICATION_METHODOLOGY
//#define USE_SIMPLER_SWITCH_MODIFICATION_METHODOLOGY

/* This set will contain IDs of the routing wires we will cut */
static std::set<int> cut_node_set;
static int* y_cuts = 0;
static int*** interposer_node_loc; 
static int* interposer_nodes; 

void find_all_CHANY_wires_that_cross_the_interposer(int nodes_per_chan, int** rr_nodes_that_cross, int* num_rr_nodes_that_cross);
void expand_rr_graph(int* rr_nodes_that_cross, int num_rr_nodes_that_cross, int nodes_per_chan);
void delete_rr_connection(int src_node, int dst_node);
void create_rr_connection(int src_node, int dst_node, int connection_switch_index);

/* ---------------------------------------------------------------------------
 * 				Functions begin 
 * ---------------------------------------------------------------------------*/

// reverse_map[inode] will be an array of routing nodes that drive inode.
int **reverse_map = 0;

void alloc_and_build_reverse_map(int num_all_rr_nodes)
{
	reverse_map = (int **) my_malloc(num_all_rr_nodes * sizeof(int*));
	int i,j, inode, iedge, dst_node;
	for(i=0; i<num_all_rr_nodes; i++)
	{
		reverse_map[i] = (int*) my_malloc(rr_node[i].fan_in * sizeof(int));
	}
	
	for(i=0;i<num_all_rr_nodes;i++)
	{
		for(j=0;j<rr_node[i].fan_in;j++)
		{
			reverse_map[i][j]=-1;
		}
	}

	for(inode=0; inode<num_all_rr_nodes; inode++)
	{	// for each routing node
		
		for(iedge = 0; iedge < rr_node[inode].num_edges; iedge++)
		{	// for each of its outgoing edges

			dst_node = rr_node[inode].edges[iedge];
			
			if(dst_node == -1)
			{
				continue;
			}
			else
			{
				// find the first available slot
				for(i=0; i<rr_node[dst_node].fan_in; i++)
				{
					if(reverse_map[dst_node][i]==-1)
						break;
				}

				// if i==rr_node[dst_node].fan_in, that means, we are trying to insert something 
				// into the reverse_map of a node, but there's no more space to do that
				// that means that number of fanins ( ".fan_in" ) was not correct in the first place.
				assert(i < rr_node[dst_node].fan_in);

				// add to the reverse map
				reverse_map[dst_node][i] = inode;
			}
		}
	}
}

void free_reverse_map(int num_all_rr_nodes)
{
	int i;
	for(i=0; i<num_all_rr_nodes; i++)
	{
		free(reverse_map[i]);
	}
	free(reverse_map);
}


void print_fanins_and_fanouts_of_rr_node(int inode)
{
	printf("Fanouts of Node %d: ", inode);
	int i;
	for(i=0; i<rr_node[inode].num_edges;++i)
	{
		printf("%d,",rr_node[inode].edges[i]);
	}
	printf("\n");

	printf("Fanins of Node %d: ", inode);
	for(i=0; i<rr_node[inode].fan_in; ++i)
	{
		printf("%d,", reverse_map[inode][i]);
	}
	printf("\n");
}

/*
 * Description: Helper function that dumps rr_graph connections
 * 
 * Returns: none.
 */
void dump_rr_graph_connections(FILE* fp)
{

	// dump the whole rr_graph
	int iedge, inode, dst_node;
	char* type[7] = {"SOURCE", "SINK", "IPIN", "OPIN", "CHANX", "CHANY", "INTRA_CLUSTER_EDGE"};


	for(inode=0; inode<num_rr_nodes;++inode)
	{
		for(iedge = 0; iedge < rr_node[inode].num_edges; iedge++)
		{
			dst_node = rr_node[inode].edges[iedge];
			const char* inode_dir = (rr_node[inode].direction == INC_DIRECTION) ? "INC" : "DEC";
			const char* dst_node_dir = (rr_node[dst_node].direction == INC_DIRECTION) ? "INC" : "DEC";

			fprintf(fp, "(%s,%d,%d,%d,%d,%d,%s) \t ", type[rr_node[inode].type], inode, rr_node[inode].xlow, rr_node[inode].xhigh, rr_node[inode].ylow, rr_node[inode].yhigh, inode_dir );
			fprintf(fp, "(%s,%d,%d,%d,%d,%d,%s) \t ", type[rr_node[dst_node].type], dst_node, rr_node[dst_node].xlow, rr_node[dst_node].xhigh, rr_node[dst_node].ylow, rr_node[dst_node].yhigh, dst_node_dir );
//			fprintf(fp, "(%s,%d,%d,%d,%d,%s) \t ", type[rr_node[inode].type], rr_node[inode].xlow, rr_node[inode].xhigh, rr_node[inode].ylow, rr_node[inode].yhigh, inode_dir );
//			fprintf(fp, "(%s,%d,%d,%d,%d,%s) \t ", type[rr_node[dst_node].type], rr_node[dst_node].xlow, rr_node[dst_node].xhigh, rr_node[dst_node].ylow, rr_node[dst_node].yhigh, dst_node_dir );

			int switch_index = rr_node[inode].switches[iedge];
			fprintf(fp, "switch_delay[%d]=%g\n",switch_index, switch_inf[switch_index].Tdel);
		}
	}

/*	
	// just to show that the bug#4 does exist
	alloc_and_build_reverse_map(num_rr_nodes);
	int cuts[] = {6,12,18};
	int i=0;
	bool found_driver_below_cut=false, found_driver_above_cut=false;
	int driver_above_cut=-1, driver_below_cut=-1;

	for(i=0; i<3; i++)
	{
		for(inode=0; inode<num_rr_nodes;++inode)
		{
			if(rr_node[inode].type==CHANY && rr_node[inode].ylow <= cuts[i] && rr_node[inode].yhigh > cuts[i])
			{
				for(iedge = 0; iedge < rr_node[inode].fan_in; iedge++)
				{
					int node_before = reverse_map[inode][iedge];
					if(node_before==-1)
						continue;

					if(rr_node[node_before].type!=CHANY && rr_node[node_before].ylow <= cuts[i])
					{
						found_driver_below_cut = true;
						driver_below_cut = node_before;
					}
					else if(rr_node[node_before].type!=CHANY && rr_node[node_before].ylow > cuts[i])
					{
						found_driver_above_cut = true;
						driver_above_cut = node_before;
					}

					if(found_driver_above_cut && found_driver_below_cut)
					{
						fprintf(fp, "(%s,%d,%d,%d,%d,%d,%s) \t ", type[rr_node[driver_below_cut].type], driver_below_cut, rr_node[driver_below_cut].xlow, rr_node[driver_below_cut].xhigh, rr_node[driver_below_cut].ylow, rr_node[driver_below_cut].yhigh, (rr_node[driver_below_cut].direction == INC_DIRECTION) ? "INC" : "DEC" );
						fprintf(fp, "(%s,%d,%d,%d,%d,%d,%s) \t ", type[rr_node[inode].type], inode, rr_node[inode].xlow, rr_node[inode].xhigh, rr_node[inode].ylow, rr_node[inode].yhigh, (rr_node[inode].direction == INC_DIRECTION) ? "INC" : "DEC" );
						fprintf(fp, "\nAND\n");
						fprintf(fp, "(%s,%d,%d,%d,%d,%d,%s) \t ", type[rr_node[driver_above_cut].type], driver_above_cut, rr_node[driver_above_cut].xlow, rr_node[driver_above_cut].xhigh, rr_node[driver_above_cut].ylow, rr_node[driver_above_cut].yhigh, (rr_node[driver_above_cut].direction == INC_DIRECTION) ? "INC" : "DEC" );
						fprintf(fp, "(%s,%d,%d,%d,%d,%d,%s) \t ", type[rr_node[inode].type], inode, rr_node[inode].xlow, rr_node[inode].xhigh, rr_node[inode].ylow, rr_node[inode].yhigh, (rr_node[inode].direction == INC_DIRECTION) ? "INC" : "DEC" );
						fprintf(fp, "\n\n\n");
						break;
					}
				}
			}
			found_driver_above_cut=false;
			found_driver_below_cut=false;
		}
	}
	free_reverse_map(num_rr_nodes);
*/
}


/*
 * Description: This function checks whether or not a specific connection crosses the interposer.
 * 
 * Returns: True if connection crosses the interposer. False otherwise.
 */
bool rr_edge_crosses_interposer(int src, int dst, int cut_location )
{
	// SRC node is always a vertical wire.
	// DST node can be either a horizontal or vertical wire.
	// 
	// SRC: Y_INC, Y_DEC,
	// DST: Y_INC, Y_DEC, X

	// here's trick to make my life easier.
	float cut = cut_location + 0.5;

	// start and end coordinates of src and dst dones
	// for horizontal nodes, ylow and yhigh are equal
	int src_ylow  = rr_node[src].ylow;
	int src_yhigh = rr_node[src].yhigh;
	int dst_ylow  = rr_node[dst].ylow;
	int dst_yhigh = rr_node[dst].yhigh;

	bool crosses_the_interposer = false;

	if( (rr_node[src].type==CHANY) &&
		(rr_node[dst].type==CHANX || rr_node[dst].type==CHANY))
	{
		// Case 1: SRC NODE IS VERTICAL AND INCREASING
		if(rr_node[src].direction==INC_DIRECTION)
		{
			if(rr_node[dst].type==CHANY && rr_node[dst].direction==INC_DIRECTION)
			{
				if(	(src_ylow < cut && cut < src_yhigh) ||
					(src_yhigh< cut && cut < dst_ylow))
				{
					crosses_the_interposer=true;
				}
			}
			else if(rr_node[dst].type==CHANY && rr_node[dst].direction==DEC_DIRECTION)
			{
				// this should never happen! (U-turn in vertical direction!
				//vpr_printf_info("SRC= Y INC && DST= Y DEC\n");
				assert(false);
			}
			else if(rr_node[dst].type==CHANX)
			{
				assert(dst_ylow==dst_yhigh);
				assert(dst_ylow>=src_ylow);
				if( (src_ylow < cut && cut < src_yhigh && cut < dst_ylow) ||
					(src_yhigh < cut && cut < dst_ylow))
				{
					crosses_the_interposer=true;
				}
			}
		}
		// Case 2: SRC NODE IS VERTICAL AND DECREASING
		else if(rr_node[src].direction==DEC_DIRECTION)
		{
			if(rr_node[dst].type==CHANY && rr_node[dst].direction==INC_DIRECTION)
			{
				// this should never happen! (U-turn in vertical direction!
				//vpr_printf_info("SRC= Y DEC && DST= Y INC\n");
				assert(false);
			}
			else if(rr_node[dst].type==CHANY && rr_node[dst].direction==DEC_DIRECTION)
			{
				if(	(src_ylow < cut && cut < src_yhigh) ||
					(dst_yhigh< cut && cut < src_ylow))
				{
					crosses_the_interposer=true;
				}
			}
			else if(rr_node[dst].type==CHANX)
			{
				assert(dst_ylow==dst_yhigh);
				assert(dst_ylow<=src_yhigh);
				if( (src_ylow < cut && cut < src_yhigh && dst_ylow < cut) ||
					(dst_yhigh < cut && cut < src_ylow))
				{
					crosses_the_interposer=true;
				}
			}
		}
	}

	if( (rr_node[src].type==CHANY) &&
		(rr_node[dst].type==IPIN || rr_node[dst].type==OPIN))
	{
		// OK, so sometimes we have a pin that looks like this
		// (IPIN,xlow=22,xhigh=22,ylow=13,yhigh=16,DEC)
		
		// Case 1: SRC NODE IS VERTICAL AND INCREASING
		if(rr_node[src].direction==INC_DIRECTION)
		{
			if( (src_ylow < cut && cut < src_yhigh && cut < dst_ylow) ||
				(src_yhigh < cut && cut < dst_ylow))
			{
				crosses_the_interposer=true;
			}			
		}
		// Case 2: SRC NODE IS VERTICAL AND DECREASING
		else if(rr_node[src].direction==DEC_DIRECTION)
		{
			if( (src_ylow < cut && cut < src_yhigh && dst_yhigh < cut) ||
				(dst_yhigh < cut && cut < src_ylow))
			{
				crosses_the_interposer=true;
			}
		}
	}

	return crosses_the_interposer;
}


/*
 * Description: This function cuts the edges which cross the cut for a given wire in the CHANY
 * 
 * Returns: None.
 */
void cut_rr_yedges(INP int cut_location, INP int inode)
{
	
	if(cut_node_set.find(inode)==cut_node_set.end())
	{
		cut_node_set.insert(inode);
	}

	int iedge, d_node, num_removed;
	int tmp;

	num_removed = 0;

	/* mark and remove the edges */
	for(iedge = 0; iedge < rr_node[inode].num_edges; iedge++)
	{
		d_node = rr_node[inode].edges[iedge];
		if(d_node == -1)
		{
			continue;
		}

		/* crosses the cut line, cut this edge */
		if(rr_edge_crosses_interposer(inode,d_node,cut_location))
		{
			rr_node[d_node].fan_in--;
			num_removed++;
			for(tmp = iedge; tmp+1 < rr_node[inode].num_edges; tmp++)
			{
				rr_node[inode].edges[tmp] = rr_node[inode].edges[tmp+1];
				rr_node[inode].switches[tmp] = rr_node[inode].switches[tmp+1];
			}
			rr_node[inode].edges[tmp] = -1; /* tmp = num_edges-1 */
			rr_node[inode].switches[tmp] = -1;

			iedge--; /* need to check the value just pulled into current pos */
		}
		else
		{
			/*printf(">>>> Did not cut this edge because it does not cross the boundary <<<<\n");*/
		}
	}

	/* fill the rest of the array with -1 for safety */
	for(iedge = rr_node[inode].num_edges - num_removed; iedge < rr_node[inode].num_edges; iedge++)
	{
		rr_node[inode].edges[iedge] = -1;
		rr_node[inode].switches[iedge] = -1;
	}
	rr_node[inode].num_edges -= num_removed;
	/* finished removing the edges */
}


/*
 * Description: This function cuts the edges which cross the cut for a given wire in the CHANX
 *
 * Returns: None.
 */
void cut_rr_xedges(int cut_location, int inode)
{
	
	int iedge, d_node, num_removed;
	int tmp;

	num_removed = 0;

	/* mark and remove the edges */
	for(iedge = 0; iedge < rr_node[inode].num_edges; iedge++)
	{
		d_node = rr_node[inode].edges[iedge];
		if(d_node == -1)
		{
			continue;
		}

		/* crosses the cut line, cut this edge, CHANX is always supposed to be
		 * below the cutline */
		if(	rr_node[d_node].ylow > cut_location && rr_node[d_node].type == CHANY)
		{
			rr_node[d_node].fan_in--;
			num_removed++;
			for(tmp = iedge; tmp+1 < rr_node[inode].num_edges; tmp++)
			{
				rr_node[inode].edges[tmp] = rr_node[inode].edges[tmp+1];
				rr_node[inode].switches[tmp] = rr_node[inode].switches[tmp+1];
			}
			rr_node[inode].edges[tmp] = -1; /* tmp = num_edges-1 */
			rr_node[inode].switches[tmp] = -1;

			iedge--; /* need to check the value just pulled into current pos */
		}
		else
		{
			/*printf(">>>> Did not cut this edge because it does not cross the boundary <<<<\n");*/
		}
	}

	/* fill the rest of the array with -1 for safety */
	for(iedge = rr_node[inode].num_edges - num_removed; iedge < rr_node[inode].num_edges; iedge++)
	{
		rr_node[inode].edges[iedge] = -1;
		rr_node[inode].switches[iedge] = -1;
	}
	rr_node[inode].num_edges -= num_removed;
	/* finished removing the edges */
}


/*
 * Description: This is where cutting happens!
 * Some vertical (CHANY to CHANY) connections are cut based on cutting pattern
 * Some horizontal (CHANY to CHANX) connections will also be cut
 *
 * The cutting pattern implemented here is called "uniform cut with rotation"
 *
 * Uniform means that we spread the track# that we want to cut over the width of the channel
 * For instance: if channel width is 10, and %wires_cut=60% ( | = 1 track wire, x = cut )
 * 
	x x     x x     x x
 *  | | | | | | | | | |
 * 
 * Also note that we cut wires in pairs because of the alternating INC DEC pattern of the wires
 * For instance: if you cut wires in singles (instead of pairs), and %wires_cut=50%, 
 * you could end up cutting all INC vertical wires in a channel and that causes a big Fmax hit
 *
 * Rotation means that the offset at which we start cutting will change based on X-coordinate of the channel
 * For instance, at X=0, we may start cutting at track 0, and at x=1, we may start cutting at track 4
 *
 *
 * Returns: None.
 */
void cut_connections_from_CHANY_wires
(
	int nodes_per_chan,
	int num_wires_cut,
	int cut_pos, 
	int i
)
{
	int itrack, inode, step, num_wires_cut_so_far, offset, num_chunks;

	if(num_wires_cut == 0)
	{	// nothing to do :)
		return;
	}

	offset = (i * nodes_per_chan) / nx;
	if(offset % 2) 
	{
		offset++;
	}
	offset = offset%nodes_per_chan; // to keep offset between 0 and nodes_per_chan-1

	if(num_wires_cut > 0)
	{
		// Example: if the step is 1.66, make the step 2.
		step = ceil (float(nodes_per_chan) / float(num_wires_cut));
	}
	else
	{
		step = 900900;
	}

	// cutting chunks of wires. each chunk has 2 wires (a pair)
	num_chunks = num_wires_cut/2;
	step = nodes_per_chan/num_chunks;

	if(step<=2)
	{
		// it can be proven that if %wires_cut>66%, then step=2.
		// step=2 means that there will be no gap between pairs of wires that will be cut
		// therefore, the cut pattern becomes a 'chunk cut'.
		// to avoid that, we will cap the number of chunks.
		// we require step to be greater than or equal to 3.
		step = 3;
		num_chunks = nodes_per_chan / 3;
	}

	int ichunk = 0;
	for(itrack=offset, num_wires_cut_so_far=0 ; num_wires_cut_so_far<num_wires_cut; itrack=(itrack+1)%nodes_per_chan)
	{
		for(ichunk=0; ichunk<num_chunks && num_wires_cut_so_far<num_wires_cut; ichunk++)
		{
			//printf("i=%d, j=%d, track=%d \n", i, cut_pos, itrack+(step*ichunk));
			inode = get_rr_node_index(i, cut_pos, CHANY, (itrack+(step*ichunk))%nodes_per_chan, rr_node_indices);
			cut_rr_yedges(cut_pos, inode);
			num_wires_cut_so_far++;
		}
	}
	
	// To make sure that we haven't cut the same wire twice
	assert(cut_node_set.size()==(size_t)num_wires_cut);
	cut_node_set.clear();
}

void cut_connections_from_CHANY_wires_2
(
	int nodes_per_chan
)
{

#ifdef DUMP_DEBUG_FILES
	FILE* fp = my_fopen("cutting_pattern.echo", "w", 0);
#endif

	int i, itrack, ifanout, step, offset, num_chunks, cut_counter, ifanin;
	int num_wires_cut_so_far = 0;
	int interposer_node_index;
	t_rr_node* interposer_node;

	// Find the number of wires that should be cut at each horizontal cut
	int num_wires_cut = (nodes_per_chan * percent_wires_cut) / 100;
	assert(percent_wires_cut==0 || num_wires_cut <= nodes_per_chan);

	// num_wires_cut should be an even number
	if(num_wires_cut % 2)
	{
			num_wires_cut++;
	}

	if(num_wires_cut == 0)
	{	// nothing to do :)
		return;
	}

	assert(num_wires_cut > 0);

	for(i=0; i<=nx; ++i)
	{
		offset = (i * nodes_per_chan) / nx;
		if(offset % 2) 
		{
			offset++;
		}
		offset = offset%nodes_per_chan; // to keep offset between 0 and nodes_per_chan-1

		// Example: if the step is 1.66, make the step 2.
		step = ceil (float(nodes_per_chan) / float(num_wires_cut));


		// cutting chunks of wires. each chunk has 2 wires (a pair)
		num_chunks = num_wires_cut/2;
		step = nodes_per_chan/num_chunks;

		if(step<=2)
		{
			// it can be proven that if %wires_cut>66%, then step=2.
			// step=2 means that there will be no gap between pairs of wires that will be cut
			// therefore, the cut pattern becomes a 'chunk cut'.
			// to avoid that, we will cap the number of chunks.
			// we require step to be greater than or equal to 3.
			step = 3;
			num_chunks = nodes_per_chan / 3;
		}

		for(cut_counter=0; cut_counter < num_cuts; ++cut_counter)
		{
			int ichunk = 0;
			for(itrack=offset, num_wires_cut_so_far=0 ; num_wires_cut_so_far<num_wires_cut; itrack=(itrack+1)%nodes_per_chan)
			{
				for(ichunk=0; ichunk<num_chunks && num_wires_cut_so_far<num_wires_cut; ichunk++)
				{
					int track_to_cut = (itrack+(step*ichunk))%nodes_per_chan;

					#ifdef DUMP_DEBUG_FILES
					fprintf(fp, "Cutting interposer node at i=%d, j=%d, track=%d \n", i, y_cuts[cut_counter], track_to_cut);
					#endif

					interposer_node_index = interposer_node_loc[i][cut_counter][track_to_cut];
					interposer_node = &rr_node[interposer_node_index];

					// cut all fanout connections of the interposer node
					for(ifanout=0; ifanout < interposer_node->num_edges; ++ifanout)
					{
						delete_rr_connection(interposer_node_index, interposer_node->edges[ifanout]);
						--ifanout;
					}
					assert(rr_node[interposer_node_index].num_edges==0);

					// cut all fanin connections of the interposer node
					for(ifanin=0; ifanin < interposer_node->fan_in; ++ifanin)
					{
						int fanin_node_index = reverse_map[interposer_node_index][ifanin];
						delete_rr_connection(fanin_node_index, interposer_node_index);
						--ifanin;
					}
					assert(rr_node[interposer_node_index].fan_in==0);

					num_wires_cut_so_far++;
				}
			}
		}
	}

	assert(num_wires_cut_so_far==num_wires_cut);

#ifdef DUMP_DEBUG_FILES
	fclose(fp);
#endif

}

/*
 * Description: This is where cutting happens!
 * CHANX to CHANY connections will be cut if CHANX wire is below the cut and the CHANY wire is above the interposer
 *
 * Returns: None.
 */
void cut_connections_from_CHANX_wires(int i, int cut_pos, int nodes_per_chan)
{
	int itrack, inode;

	if(cut_pos + 1 >= ny)
	{
		return;
	}

	// From CHANX to CHANY, cut only the edges at the switches
	if(0 < i && i < nx)
	{
		for(itrack = 0; itrack < nodes_per_chan; itrack++)
		{
			inode = get_rr_node_index(i, cut_pos, CHANX, itrack, rr_node_indices);

			// printf("Going to cut Horizontal (X) edges from: i=%d, j=%d, track=%d \n", i, cut_pos, itrack);
			cut_rr_xedges(cut_pos, inode);
		}
	}
}


/*
 * Description: Takes care of cutting horizontal and vertical connections at the cut
 *
 * Returns: None.
 */
void cut_rr_graph_edges_at_cut_locations(int nodes_per_chan)
{
	int cut_step; // The interval at which the cuts should be made
	int counter;  // Number of cuts already made
	int i, j;     // horizontal and vertical coordinate numbers
	int num_wires_cut;

	// Find the number of wires that should be cut at each horizontal cut
	num_wires_cut = (nodes_per_chan * percent_wires_cut) / 100;
	assert(percent_wires_cut==0 || num_wires_cut <= nodes_per_chan);

	// num_wires_cut should be an even number
	if(num_wires_cut % 2)
	{
			num_wires_cut++;
	}

	printf("Info: cutting %d wires when channel width is %d\n", num_wires_cut, nodes_per_chan);

	counter = 0;
	cut_step = ny / (num_cuts + 1);
	for(j=cut_step; j<ny && counter<num_cuts; j+=cut_step, counter++)
	{
		for(i = 0; i <= nx; i++)
		{
			// 1. cut num_wires_cut wires at (x,y)=(i,j).
			cut_connections_from_CHANY_wires(nodes_per_chan, num_wires_cut, j, i);

			// 2. cut all CHANX wires connecting to CHANY wires on the other side of the interposer
			cut_connections_from_CHANX_wires(i, j, nodes_per_chan);
		}
	}
	assert(counter == num_cuts);
}


/*
 * Description: This function traverses the whole rr_graph
 * For every connection that crosses the interposer, it increases the switch delay
 *
 * Returns: None.
 */
void increase_delay_rr_edges()
{
	int iedge, inode, dst_node, cut_counter, cut_step;
	int cut_pos;

	cut_step = ny / (num_cuts+1);

	for(inode=0; inode<num_rr_nodes;++inode)
	{
		// we only increase the delay of connections from CHANY nodes to other nodes.
		if(rr_node[inode].type==CHANY)
		{
			for(iedge = 0; iedge < rr_node[inode].num_edges; iedge++)
			{
				dst_node = rr_node[inode].edges[iedge];

				if(dst_node==-1)
				{	// if it's a connection that's already cut, you don't need to increase its delay
					continue;
				}

				// see if the connection crosses any of the cuts
				cut_counter = 0;
				for(cut_pos = cut_step; cut_pos < ny && cut_counter < num_cuts; cut_counter++, cut_pos+=cut_step)
				{
					if(rr_edge_crosses_interposer(inode,dst_node,cut_pos))
					{
						rr_node[inode].switches[iedge] = increased_delay_edge_map[rr_node[inode].switches[iedge]];
						break;
					}
				}
			}
		}
	}

}

void increase_delay_rr_edges_2(int nodes_per_chan)
{
	int inode, i, j;
	int old_switch, new_switch;
	int num_interposer_nodes = (nx+1)*(num_cuts)*(nodes_per_chan);
	for(i=0; i<num_interposer_nodes; ++i)
	{
		inode = interposer_nodes[i];

		/*
		for(j=0; j < rr_node[inode].num_edges; ++j)
		{
			old_switch = rr_node[inode].switches[j]
			rr_node[inode].switches[j] = increased_delay_edge_map[current_switch];			
		}
		*/

		for(j=0; j < rr_node[inode].fan_in; ++j)
		{
			int fanin_node_id = reverse_map[inode][j];
			int cnt;
			for(cnt=0; cnt<rr_node[fanin_node_id].num_edges && rr_node[fanin_node_id].edges[cnt]!=inode; ++cnt);
			old_switch = rr_node[fanin_node_id].switches[cnt];
			new_switch = increased_delay_edge_map[old_switch];
			rr_node[fanin_node_id].switches[cnt] = new_switch;

			// printf("Changing from switch_delay[%d]=%g to switch_delay[%d]=%g\n", old_switch, switch_inf[old_switch].Tdel, new_switch, switch_inf[new_switch].Tdel);
		}
	}
}

/* 
 * Function that does num_cuts horizontal cuts to the chip,
 * percent_wires_cut% of the wires crossing these cuts are removed
 * (the edges which cross the cut are removed) and the remaining
 * wires on this section have their delay increased by delay_increase (ns)
 */
void modify_rr_graph_for_interposer_based_arch
(
	int nodes_per_chan,
	enum e_directionality directionality
)
{

	if(directionality == BI_DIRECTIONAL) /* Ignored for now TODO */
		return;

#ifdef	DUMP_DEBUG_FILES
	// Before doing anything, let's dump the vertical track connections in the rr_graph
	FILE* fp = my_fopen("before_cutting.txt", "w", 0);
	dump_rr_graph_connections(fp);
	fclose(fp);
#endif

#ifdef USE_NODE_DUPLICATION_METHODOLOGY

	// 0. Populate the y-coordinate of cut locations
	y_cuts = (int*) my_malloc(num_cuts * sizeof(int));
	int cut_step = ny / (num_cuts+1);
	int cut_counter = 0;
	for(cut_counter = 0; cut_counter < num_cuts; ++cut_counter)
	{
		y_cuts[cut_counter] = (cut_counter+1)*cut_step;
	}


	int *rr_nodes_that_cross = 0;
	int num_rr_nodes_that_cross;
	
	// 1. find all CHANY wires that cross the interposer
	find_all_CHANY_wires_that_cross_the_interposer(nodes_per_chan, &rr_nodes_that_cross, &num_rr_nodes_that_cross);
	
	// 2. This the tough part: add new rr_nodes and fix up fanins and fanouts and switches
	expand_rr_graph(rr_nodes_that_cross, num_rr_nodes_that_cross, nodes_per_chan); 

	// 3. cut some of the wires
	cut_connections_from_CHANY_wires_2(nodes_per_chan);

	// 4. increase the delay of interposer nodes that were not cut
	increase_delay_rr_edges_2(nodes_per_chan);

#endif

#ifdef USE_SIMPLER_SWITCH_MODIFICATION_METHODOLOGY
	// 1. Cut as many wires as we need to.
	//    This will cut %wires_cut of vertical connections at the interposer. 
	//    It will also cut connections from CHANX wires below the interposer to CHANY wires above the interposer
	cut_rr_graph_edges_at_cut_locations(nodes_per_chan);

	// 2. Increase the delay of all the remaining tracks that pass the interposer
	//    by increasing the switch delays
	increase_delay_rr_edges();
#endif


#ifdef	DUMP_DEBUG_FILES
	// dump after all rr_Graph modifications are done
	FILE* fp2 = my_fopen("after_cutting.txt", "w", 0);
	dump_rr_graph_connections(fp2);
	fclose(fp2);
#endif

#ifdef USE_NODE_DUPLICATION_METHODOLOGY
	// free stuff
	free(y_cuts);

	// free stuff
	free_reverse_map(num_rr_nodes);
	free(interposer_nodes);

	// free stuff
	int i,j;
	for(i=0; i<nx+1; ++i)
	{
		for(j=0; j<num_cuts; ++j)
		{
			free(interposer_node_loc[i][j]);
		}
	}
	for(i=0; i<nx+1; ++i)
	{
		free(interposer_node_loc[i]);
	}
	free(interposer_node_loc);
#endif 
}



//####################################
// Here's where i attempt to modify the rr_graph in a whole different way:
// First, find all rr_nodes that cross the interposer.
// Second, for each one create an extra node (so now we have two nodes)
// Third, change the original node to be on one side of the interposer, and the new node to be on the other side
// Fourth, connect the original node and the new node with an edge.
// There's probably so much more that needs to be done in order to keep all the indeces in correct place.
// I don't know what they are yet, but there's no way to find out until I start coding it up.

//  So, turn this:
//
//    O   O    O
//    |   |    |
//    \   |   /
//      \ | /
//        O                   <---- this is the node that's crossing the interposer (inode)
//       / \
//      /   \
//     O     O
//
// into this:
//
//
//    O   O    
//    |   |    
//    \   |   
//      \ | 
//        |
//     O  O                   <---- this is the new node (new_node)
//      \ |
//        O                   <---- this is the original node (inode)
//       / \
//      /   \
//     O     O

void find_all_CHANY_wires_that_cross_the_interposer(int nodes_per_chan, int** rr_nodes_that_cross, int* num_rr_nodes_that_cross)
{
	int inode, cut_pos, cut_counter;
	*num_rr_nodes_that_cross = 0;

	// at the very most (if all vertical wires in the channel cross the interposer)
	// the number of nodes that cross the interposer will be: num_nodes_per_channel * nx * num_cuts
	int max_num_nodes_that_cross = nodes_per_chan*(nx+1)*num_cuts;
	*rr_nodes_that_cross = (int*) my_malloc(max_num_nodes_that_cross * sizeof(int));
	for(inode=0; inode<max_num_nodes_that_cross; ++inode)
	{
		(*rr_nodes_that_cross)[inode] = -1;
	}

	for(inode=0; inode<num_rr_nodes;++inode)
	{
		if(rr_node[inode].type==CHANY)
		{
			for(cut_counter=0; cut_counter < num_cuts; cut_counter++)
			{
				cut_pos = y_cuts[cut_counter];
				if(rr_node[inode].ylow <= cut_pos && rr_node[inode].yhigh > cut_pos)
				{
					(*rr_nodes_that_cross)[*num_rr_nodes_that_cross] = inode;
					(*num_rr_nodes_that_cross)++;
					break;
				}
			}
		}
	}
	
	// DEBUG MESSAGES
	/*
	printf("Found %d CHANY nodes that cross the interposer:\n", *num_rr_nodes_that_cross);
	int i;
	for(i=0;i<*num_rr_nodes_that_cross;++i)
	{
		inode = (*rr_nodes_that_cross)[i];
		printf("Node: %d,%d,%d,%d,%d\n", inode, rr_node[inode].xlow, rr_node[inode].xhigh, rr_node[inode].ylow, rr_node[inode].yhigh);
	}
	*/
	
}


// this function deletes the connection between src node and dst node
// it will take care of updating all necessary data-structures
// if no connection exists between src and dst, it will return without doing anything
void delete_rr_connection(int src_node, int dst_node)
{
	// Debug
	/*
	printf("before deleting edge from %d to %d\n", src_node, dst_node);
	print_fanins_and_fanouts_of_rr_node(src_node);
	print_fanins_and_fanouts_of_rr_node(dst_node);
	*/

	// 0. return if the connection doesn't exist
	int i, counter;
	bool connected = false;
	for(i=0; i<rr_node[src_node].num_edges; ++i)
	{
		if(rr_node[src_node].edges[i]==dst_node)
		{
			connected = true;
		}
	}
	if(!connected)
	{
		return;
	}


	// 1. take care of the source node side
	// this will work fine even if num_src_fanouts_after_cutting is 0
	int num_src_fanouts_before_cutting = rr_node[src_node].num_edges;
	int num_src_fanouts_after_cutting  = num_src_fanouts_before_cutting - 1;

	int   *temp_src_edges    = (int*)   my_malloc(num_src_fanouts_after_cutting * sizeof(int));
	short *temp_src_switches = (short*) my_malloc(num_src_fanouts_after_cutting * sizeof(short));
	counter = 0;
	for(i=0; i < num_src_fanouts_before_cutting; ++i)
	{
		if(rr_node[src_node].edges[i]!=dst_node)
		{
			temp_src_edges[counter]    = rr_node[src_node].edges[i];
			temp_src_switches[counter] = rr_node[src_node].switches[i];
			counter++;
		}
	}
	assert(counter==num_src_fanouts_after_cutting);
	assert(num_src_fanouts_after_cutting >= 0);

	rr_node[src_node].num_edges = num_src_fanouts_after_cutting;
	free(rr_node[src_node].edges);
	free(rr_node[src_node].switches);
	rr_node[src_node].edges = temp_src_edges;
	rr_node[src_node].switches = temp_src_switches;


	// 2. take care of the destination node side
	// this will work fine even if num_dst_fanins_after_cutting is 0
	int num_dst_fanins_before_cutting = rr_node[dst_node].fan_in;
	int num_dst_fanins_after_cutting  = num_dst_fanins_before_cutting - 1;
	int* temp_dst_fanins = (int*) my_malloc(num_dst_fanins_after_cutting * sizeof(int));
	i=0;
	counter=0;
	for(i=0; i < num_dst_fanins_before_cutting; ++i)
	{
		if(reverse_map[dst_node][i]!=src_node)
		{
			temp_dst_fanins[counter] = reverse_map[dst_node][i];
			counter++;
		}
	}
	assert(counter==num_dst_fanins_after_cutting);

	free(reverse_map[dst_node]);
	reverse_map[dst_node] = temp_dst_fanins;

	rr_node[dst_node].fan_in = num_dst_fanins_after_cutting;
	assert(rr_node[dst_node].fan_in>=0);

	// Debug
	/*
	printf("after deleting edge from %d to %d\n", src_node, dst_node);
	print_fanins_and_fanouts_of_rr_node(src_node);
	print_fanins_and_fanouts_of_rr_node(dst_node);
	*/
}

// this function creates a new connection from SRC to DST node
// it will take care of updating all necessary data-structures
// the connection will use a switch with ID of connection_switch_index
// if connection from src to dst already exists, it returns without doing anything
void create_rr_connection(int src_node, int dst_node, int connection_switch_index)
{
	// Debug
	/*
	printf("before creating edge from %d to %d\n", src_node, dst_node);
	print_fanins_and_fanouts_of_rr_node(src_node);
	print_fanins_and_fanouts_of_rr_node(dst_node);
	*/

	// 0. if connection already exists, return
	int i;
	bool already_connected = false;
	for(i=0; i<rr_node[src_node].num_edges; ++i)
	{
		if(rr_node[src_node].edges[i]==dst_node)
		{
			already_connected = true;
		}
	}
	if(already_connected)
	{
		return;
	}

	// 1. take care of the source node side
	// realloc will behave like malloc if pointer was NULL before
	rr_node[src_node].num_edges++;
	int num_src_fanouts_after_new_connection = rr_node[src_node].num_edges;
	assert(num_src_fanouts_after_new_connection > 0);
	rr_node[src_node].edges =    (int*)   my_realloc(rr_node[src_node].edges,    sizeof(int)*(num_src_fanouts_after_new_connection));
	rr_node[src_node].switches = (short*) my_realloc(rr_node[src_node].switches, sizeof(int)*(num_src_fanouts_after_new_connection));
	rr_node[src_node].edges[num_src_fanouts_after_new_connection-1] = dst_node;
	rr_node[src_node].switches[num_src_fanouts_after_new_connection-1] = connection_switch_index;

	// 2. take care of the dst node side
	rr_node[dst_node].fan_in++;
	int num_dst_fanins_after_new_connection = rr_node[dst_node].fan_in;
	reverse_map[dst_node] = (int*)my_realloc(reverse_map[dst_node], sizeof(int)*(num_dst_fanins_after_new_connection));
	reverse_map[dst_node][num_dst_fanins_after_new_connection-1] = src_node;

	// Debug
	/*
	printf("after creating edge from %d to %d\n", src_node, dst_node);
	print_fanins_and_fanouts_of_rr_node(src_node);
	print_fanins_and_fanouts_of_rr_node(dst_node);
	*/
}

void expand_rr_graph(int* rr_nodes_that_cross, int num_rr_nodes_that_cross, int nodes_per_chan)
{
	// note to self: also see: 
	// rr_node_to_rt_node
	// rr_node_route_inf

	int inode;
	int i, j, k, cnt, interposer_node_counter;

	// this is where we need to add a switch with EXTRA delay (this is the one that crosses the interposer)
	// EHSAN: this is not right. i need to find out the switch index for a correct CHANY_to_CHANY connection
	int correct_index_of_CHANY_to_CHANY_switch = 0;
	int zero_delay_switch_index = 4;


	// rr_node used to be rr_node[0..num_rr_nodes-1]
	// now, it will be bigger: rr_node[0.. num_rr_nodes+num_rr_nodes_that_cross-1]
	// the indeces of the newly created nodes will be: num_rr_nodes .. num_rr_nodes+num_rr_nodes_that_cross-1
	// we also decided to add nodes for the interposer. So, we are adding 1 extra node per vertical track (CHANY) at the cut locations
	// The indeces of the interposer nodes will be [num_rr_nodes+num_rr_nodes_that_cross .. num_rr_nodes+num_rr_nodes_that_cross+num_interposer_nodes-1]
	int num_vertical_channels = nx+1;
	int num_interposer_nodes = num_cuts * num_vertical_channels * nodes_per_chan;

	// expand
	rr_node = (t_rr_node *)my_realloc(rr_node, sizeof(t_rr_node)*(num_rr_nodes+num_rr_nodes_that_cross+num_interposer_nodes));
	//rr_node = (t_rr_node *)my_realloc(rr_node, sizeof(t_rr_node)*(num_rr_nodes+num_rr_nodes_that_cross));
	// initialize the new nodes to some initial state
	
	for(inode=num_rr_nodes; inode<num_rr_nodes+num_rr_nodes_that_cross+num_interposer_nodes; ++inode)
	{
		rr_node[inode].xlow = -1;
		rr_node[inode].xhigh = -1;
		rr_node[inode].ylow = -1;
		rr_node[inode].yhigh = -1;
		rr_node[inode].z=-1;
		rr_node[inode].ptc_num = -1;
		rr_node[inode].cost_index = -1;
		rr_node[inode].occ = -1;
		rr_node[inode].capacity = -1;
		
		// it's important to make fan_in=0 so that the reverse map doesn't freak out
		rr_node[inode].fan_in = 0;
		rr_node[inode].num_edges = 0;
		rr_node[inode].edges=0;
		rr_node[inode].switches=0;

		rr_node[inode].R=0;
		rr_node[inode].C=0;
		rr_node[inode].num_wire_drivers=0;
		rr_node[inode].num_opin_drivers=0;
		rr_node[inode].prev_node=0;
		rr_node[inode].prev_edge=0;
		rr_node[inode].net_num=0;
		rr_node[inode].pb_graph_pin=0;
		rr_node[inode].tnode=0;
		rr_node[inode].pack_intrinsic_cost=0.0;
	}

	alloc_and_build_reverse_map(num_rr_nodes+num_rr_nodes_that_cross+num_interposer_nodes);
	//alloc_and_build_reverse_map(num_rr_nodes+num_rr_nodes_that_cross);

	// for any wire that crosses the interposer, cut into 2 wires
	// 1 wire below the cut, and 1 wire above the cut
	for(i=0; i<num_rr_nodes_that_cross; ++i)
	{
		int original_node_index = rr_nodes_that_cross[i];
		int new_node_index = num_rr_nodes+i;

		t_rr_node* original_node = &rr_node[original_node_index];
		t_rr_node* new_node = &rr_node[new_node_index];

		// find which cut goes through the original node
		int cut_counter = 0, cut_pos = 0;
		for(cut_counter = 0; cut_counter < num_cuts; cut_counter++)
		{
			cut_pos = y_cuts[cut_counter];
			if( original_node->ylow <= cut_pos && cut_pos < original_node->yhigh )
			{
				break;
			}
		}

		// remember the length of the original_node before it is cut to 2 pieces.
		int original_wire_len_before_cutting = original_node->yhigh - original_node->ylow + 1;

		// the y-coordinates should be fixed
		if(original_node->direction == INC_DIRECTION)
		{
			new_node->yhigh = original_node->yhigh;
			new_node->ylow = cut_pos+1;
			original_node->yhigh = cut_pos;
			// don't need to change original_node->ylow	
		}
		else if(original_node->direction == DEC_DIRECTION)
		{
			new_node->ylow = original_node->ylow;
			new_node->yhigh = cut_pos;
			original_node->ylow = cut_pos+1;
			// don't need to change original_node->yhigh
		}

		// the following attributes of the new node should be the same as the original node
		new_node->xlow = original_node->xlow;
		new_node->xhigh = original_node->xhigh;
		new_node->ptc_num = original_node->ptc_num;
		
		new_node->cost_index = original_node->cost_index;
		new_node->occ = original_node->occ;
		new_node->capacity = original_node->capacity;

		assert(original_node->type==CHANY);
		new_node->type = original_node->type;

		// Figure out how to distribute the R and C between the two wires
		int original_wire_len_after_cutting = original_node->yhigh - original_node->ylow + 1;
		int new_wire_len = new_node->yhigh - new_node->ylow + 1;
		assert(original_wire_len_before_cutting == original_wire_len_after_cutting+new_wire_len);
		new_node->R = original_node->R;
		new_node->C = ( (float)(new_wire_len) / (float)(original_wire_len_before_cutting) ) * original_node->C;
		original_node->C = ( (float)(original_wire_len_after_cutting) / (float)(original_wire_len_before_cutting) ) * original_node->C;

		new_node->direction = original_node->direction;
		new_node->drivers = original_node->drivers;
		new_node->num_wire_drivers = original_node->num_wire_drivers;
		new_node->num_opin_drivers = original_node->num_opin_drivers;

		// ######## Update fanouts of the original_node and new_node
		int num_org_node_fanouts_before_transformations = original_node->num_edges;
		int num_org_node_fanins_before_transformations = original_node->fan_in;

		for(cnt=0; cnt<original_node->num_edges; ++cnt)
		{
			int dnode = original_node->edges[cnt];
			short iswitch = original_node->switches[cnt];

			if( (original_node->direction==INC_DIRECTION && rr_node[dnode].ylow > cut_pos) ||
				(original_node->direction==DEC_DIRECTION && rr_node[dnode].ylow <= cut_pos))
			{
				if(original_node->direction==DEC_DIRECTION && rr_node[dnode].ylow == cut_pos && rr_node[dnode].type==CHANX)
				{
				}
				else
				{
					// this should be removed from fanout set of original_node 
					// and should be added to fanouts of the new_node
					create_rr_connection(new_node_index,dnode, iswitch);
					delete_rr_connection(original_node_index,dnode);
					cnt--;
				}
			}
		}

		// ######## Hook up original_node to new_node
		create_rr_connection(original_node_index,new_node_index,correct_index_of_CHANY_to_CHANY_switch);

		// ######## Update fanins of the original_node and new_node
		for(cnt=0; cnt < original_node->fan_in; ++cnt)
		{
			// for every fan-in of the current original_node

			int fanin_node_index = reverse_map[original_node_index][cnt];
			t_rr_node* fanin_node = &rr_node[fanin_node_index];

			if(	(original_node->direction==INC_DIRECTION && fanin_node->yhigh > cut_pos) ||
				(original_node->direction==DEC_DIRECTION && fanin_node->ylow  <= cut_pos ) ||
				(original_node->direction==INC_DIRECTION && fanin_node->yhigh == cut_pos && fanin_node->type == CHANX)
				)
			{
				// fanin_node should be removed from original_node fanin set
				// and should now feed the new_node instead of the original_node
				// use the same switch for the new connection
				int i;
				for(i=0; i<fanin_node->num_edges && fanin_node->edges[i]!=original_node_index; ++i);
				create_rr_connection(fanin_node_index,new_node_index, fanin_node->switches[i]);
				delete_rr_connection(fanin_node_index,original_node_index);
				cnt--;
			}
		}

		int num_org_node_fanouts_after_transformations = original_node->num_edges;
		int num_org_node_fanins_after_transformations = original_node->fan_in;
		int num_new_node_fanouts_after_transformations = new_node->num_edges;
		int num_new_node_fanins_after_transformations = new_node->fan_in;

		// +2 is because of 1 fanout and 1 fanin added by connecting original_node to new_node
		assert( (num_org_node_fanouts_before_transformations + num_org_node_fanins_before_transformations + 2) ==
				(num_org_node_fanouts_after_transformations + num_org_node_fanins_after_transformations +
				num_new_node_fanouts_after_transformations + num_new_node_fanins_after_transformations)
				);

		// The rest of the member variables are not used yet (they are 0 in the original_node)
		new_node->prev_node=0;
		new_node->prev_edge=0;
		new_node->net_num=0;
		new_node->pb_graph_pin=0;
		new_node->tnode=0;
		new_node->pack_intrinsic_cost=0.0;
	}

	// update the num_rr_nodes so far
	num_rr_nodes += num_rr_nodes_that_cross;

	//############# Begin: Legality Check ##################################################################
	for(inode=0; inode<num_rr_nodes; ++inode)
	{
		if(rr_node[inode].type==CHANY)
		{
			int cut_counter, cut_pos;
			for(cut_counter=0; cut_counter < num_cuts; cut_counter++)
			{
				cut_pos = y_cuts[cut_counter];
				if(rr_node[inode].ylow <= cut_pos && rr_node[inode].yhigh > cut_pos)
				{
					vpr_printf(TIO_MESSAGE_ERROR, "in expand_rr_graph: rr_node %d crosses the cut at y=%d\n", inode, cut_pos);
					exit(1);
				}

				for(i=0; i<rr_node[inode].num_edges; ++i)
				{
					int dnode = rr_node[inode].edges[i];
					
					if(	rr_node[dnode].type==SINK   || 
						rr_node[dnode].type==SOURCE || 
						rr_node[dnode].type==IPIN   || 
						rr_node[dnode].type==OPIN)
					{
						if( (rr_node[inode].type==INC_DIRECTION && rr_node[inode].yhigh <= cut_pos && rr_node[dnode].ylow > cut_pos) ||
							(rr_node[inode].type==DEC_DIRECTION && rr_node[inode].ylow > cut_pos && rr_node[dnode].yhigh <= cut_pos))
						{
							vpr_printf(TIO_MESSAGE_ERROR,
								"in expand_rr_graph: rr_node %d tries to connect to a pin on the other side of the cut at y=%d\n", inode, cut_pos);
							exit(1);
						}
					}
				}
			}
		}
	}
	//############# End: Legality Check   ##################################################################
	

	//############# BEGIN: Add interposer nodes   ##########################################################
	// allocate stuff
	// interposer_nodes[x][y][pct]
	interposer_node_loc = (int***) my_malloc(num_vertical_channels * sizeof(int**));
	for(i=0; i<num_vertical_channels; ++i)
	{
		interposer_node_loc[i] = (int**)my_malloc(num_cuts * sizeof(int*));
	}
	for(i=0; i<num_vertical_channels; ++i)
	{
		for(j=0; j<num_cuts; ++j)
		{
			interposer_node_loc[i][j] = (int*)my_malloc(nodes_per_chan * sizeof(int));
		}
	}
	for(i=0; i<num_vertical_channels; ++i)
	{
		for(j=0; j<num_cuts; ++j)
		{
			for(k=0; k<nodes_per_chan; ++k)
			{
				interposer_node_loc[i][j][k] = -1;
			}
		}
	}

	// allocate stuff
	interposer_nodes = (int*)my_malloc(num_interposer_nodes * sizeof(int));

	// remember that interposer node IDs will be at 
	// [num_rr_nodes+num_rr_nodes_that_cross .. num_rr_nodes+num_rr_nodes_that_cross+num_interposer_nodes-1]
	// but since we already did num_rr_nodes += num_rr_nodes_that_cross, so the indeces will be at:
	// [num_rr_nodes .. num_rr_nodes+num_interposer_nodes-1]
	interposer_node_counter = 0;
	int interposer_node_id  = num_rr_nodes;
	for(inode=0; inode<num_rr_nodes; ++inode)
	{
		t_rr_node* node = &rr_node[inode];
		int cut_counter = 0;
		for(cut_counter = 0; cut_counter < num_cuts; cut_counter++)
		{
			int cut_pos = y_cuts[cut_counter];

			if(node->type==CHANY && (node->ylow==cut_pos || node->yhigh==cut_pos))
			{
				if(node->direction==INC_DIRECTION)
				{
					// by this point, we should not have any wires that cross a cut
					// so, the yhigh should at most be cut_pos
					assert(node->yhigh==cut_pos);
					assert(node->xlow==node->xhigh);

					rr_node[interposer_node_id].xlow=node->xlow;
					rr_node[interposer_node_id].xhigh=node->xhigh;
					rr_node[interposer_node_id].ylow=cut_pos;
					rr_node[interposer_node_id].yhigh=cut_pos;
					rr_node[interposer_node_id].z=node->z;
					//////////////////////////////rr_node[interposer_node_id].ptc_num=node->ptc_num+nodes_per_chan;
					rr_node[interposer_node_id].ptc_num=node->ptc_num;

					rr_node[interposer_node_id].cost_index=node->cost_index;
					rr_node[interposer_node_id].occ=0;
					rr_node[interposer_node_id].capacity=1;
					rr_node[interposer_node_id].type=CHANY;
					rr_node[interposer_node_id].direction=INC_DIRECTION;
					rr_node[interposer_node_id].R=0;
					rr_node[interposer_node_id].C=0;

					// SINGLE or MULTI_BUFFERED?
					rr_node[interposer_node_id].drivers = node->drivers;

					assert(node->num_wire_drivers==0);
					assert(node->num_opin_drivers==0);
					rr_node[interposer_node_id].num_wire_drivers = 0;
					rr_node[interposer_node_id].num_opin_drivers = 0;
					rr_node[interposer_node_id].prev_node=0;
					rr_node[interposer_node_id].prev_edge=0;
					rr_node[interposer_node_id].net_num=0;
					rr_node[interposer_node_id].pb_graph_pin=0;
					rr_node[interposer_node_id].tnode=0;
					rr_node[interposer_node_id].pack_intrinsic_cost=0.0;

					create_rr_connection(inode, interposer_node_id, correct_index_of_CHANY_to_CHANY_switch);

					// all the fanouts of 'node' that are on the other side of the cut,
					// should be transfered to the interposer_node
					for(i=0; i<node->num_edges; ++i)
					{
						int ifanout = node->edges[i];
						int iswitch = node->switches[i];

						if(rr_node[ifanout].ylow > cut_pos)
						{
							// transfer the fanout
							//create_rr_connection(interposer_node_id,ifanout, iswitch);
							create_rr_connection(interposer_node_id,ifanout, zero_delay_switch_index);
							delete_rr_connection(inode, ifanout);
							--i;
						}
					}
				}
				else if(node->direction==DEC_DIRECTION)
				{
					// by this point, we should not have any wires that cross a cut
					// so, the yhigh should at most be cut_pos
					assert(node->yhigh==cut_pos);
					assert(node->xlow==node->xhigh);

					rr_node[interposer_node_id].xlow=node->xlow;
					rr_node[interposer_node_id].xhigh=node->xhigh;
					rr_node[interposer_node_id].ylow=cut_pos;
					rr_node[interposer_node_id].yhigh=cut_pos;
					rr_node[interposer_node_id].z=node->z;
					//////////////////////////////rr_node[interposer_node_id].ptc_num=node->ptc_num+nodes_per_chan;
					rr_node[interposer_node_id].ptc_num=node->ptc_num;

					rr_node[interposer_node_id].cost_index=node->cost_index;
					rr_node[interposer_node_id].occ=0;
					rr_node[interposer_node_id].capacity=1;
					rr_node[interposer_node_id].type=CHANY;
					rr_node[interposer_node_id].direction=DEC_DIRECTION;
					rr_node[interposer_node_id].R=0;
					rr_node[interposer_node_id].C=0;

					// SINGLE or MULTI_BUFFERED?
					rr_node[interposer_node_id].drivers = node->drivers;

					assert(node->num_wire_drivers==0);
					assert(node->num_opin_drivers==0);
					rr_node[interposer_node_id].num_wire_drivers = 0;
					rr_node[interposer_node_id].num_opin_drivers = 0;
					rr_node[interposer_node_id].prev_node=0;
					rr_node[interposer_node_id].prev_edge=0;
					rr_node[interposer_node_id].net_num=0;
					rr_node[interposer_node_id].pb_graph_pin=0;
					rr_node[interposer_node_id].tnode=0;
					rr_node[interposer_node_id].pack_intrinsic_cost=0.0;

					//create_rr_connection(interposer_node_id, inode, correct_index_of_CHANY_to_CHANY_switch);
					create_rr_connection(interposer_node_id, inode, zero_delay_switch_index);

					// all the fanouts of 'node' that are on the other side of the cut,
					// should be transfered to the interposer_node
					for(i=0; i<node->fan_in; ++i)
					{
						int ifanin = reverse_map[inode][i];
						for (cnt=0; cnt<rr_node[ifanin].num_edges && rr_node[ifanin].edges[cnt]!=inode; ++cnt);
						int iswitch = rr_node[ifanin].switches[cnt];

						if(rr_node[ifanin].ylow > cut_pos)
						{
							// transfer the fanin
							create_rr_connection(ifanin, interposer_node_id, iswitch);
							delete_rr_connection(ifanin, inode);
							--i;
						}
					}
				}
				else
				{
					vpr_printf(TIO_MESSAGE_ERROR,
						"rr_graph modifications for interposer based architectures currently supports unidirectional wires!\n");
					exit(1);
				}
				interposer_nodes[interposer_node_counter] = interposer_node_id;
				interposer_node_loc[node->xlow][cut_counter][node->ptc_num] = interposer_node_id;
				interposer_node_id++;
				interposer_node_counter++;
			}
		}
	}
	assert(interposer_node_counter==num_interposer_nodes);
	assert(interposer_node_id == num_rr_nodes+num_interposer_nodes);

	for(inode=0; inode<num_rr_nodes; ++inode)
	{
		t_rr_node* node = &rr_node[inode];
		int cut_counter = 0;
		for(cut_counter = 0; cut_counter < num_cuts; cut_counter++)
		{
			int cut_pos = y_cuts[cut_counter];
			if(node->type==CHANX && node->ylow==cut_pos)
			{
				assert(node->ylow==node->yhigh);  // because it's CHANX
				
				// go over all of its fanouts (it may drive a CHANY wire on the other side of the cut)
				for(i=0; i<node->num_edges; ++i)
				{
					int ifanout = node->edges[i];
					int iswitch = node->switches[i];
					t_rr_node* fanout = &rr_node[ifanout];

					if(fanout->ylow > cut_pos)
					{
						if( fanout->type==IPIN || fanout->type==OPIN ||
							fanout->type==SINK || fanout->type==SOURCE)
						{
							vpr_printf(TIO_MESSAGE_ERROR,
								"Found CHANX wire below the cut that connects to a pin above the cut!\n");
							exit(1);
						}
						else if(fanout->type==CHANX)
						{
							vpr_printf(TIO_MESSAGE_ERROR,
								"Found CHANX wire below the cut that connects to a CHANX wire above the cut!\n");
							exit(1);
						}
						else if(fanout->type==CHANY)
						{
							// transfer the fanout
							// first find out which interposer_node you want to connect to
							interposer_node_id = interposer_node_loc[fanout->xlow][cut_counter][fanout->ptc_num];
							create_rr_connection(inode, interposer_node_id, iswitch);
							create_rr_connection(interposer_node_id,ifanout, zero_delay_switch_index );
							delete_rr_connection(inode, ifanout);
							--i;
						}
					}
				}

				// go over all of its fanins (it may be driven by a CHANY wire above the cut)
				for(i=0; i<node->fan_in; ++i)
				{
					int ifanin = reverse_map[inode][i];
					t_rr_node* fanin = &rr_node[ifanin];
					for (cnt=0; cnt < fanin->num_edges && fanin->edges[cnt]!=inode; ++cnt);
					int iswitch = fanin->switches[cnt];

					if(fanin->ylow > cut_pos)
					{
						if( fanin->type==IPIN || fanin->type==OPIN ||
							fanin->type==SINK || fanin->type==SOURCE)
						{
							vpr_printf(TIO_MESSAGE_ERROR,
								"Found CHANX wire below the cut that is driven by a pin above the cut!\n");
							exit(1);
						}
						else if(fanin->type==CHANX)
						{
							vpr_printf(TIO_MESSAGE_ERROR,
								"Found CHANX wire below the cut that is driven by a CHANX wire above the cut!\n");
							exit(1);
						}
						else if(fanin->type==CHANY)
						{
							// transfer the fanout
							interposer_node_id = interposer_node_loc[fanin->xlow][cut_counter][fanin->ptc_num];
							create_rr_connection(ifanin, interposer_node_id, iswitch);
							create_rr_connection(interposer_node_id, inode, zero_delay_switch_index );
							delete_rr_connection(ifanin, inode);
							--i;
						}
					}
				}	
			}
		}
	}

	num_rr_nodes += num_interposer_nodes;

	//############# END: Add interposer nodes   ##########################################################


	//############# BEGIN: LEGALITY CHECK       ##########################################################
	// At this point, there should be no connection that crosses the interposer, UNLESS it goes through
	// an interposer node
	t_rr_node *node, *fanout_node; 
	for(inode=0; inode<num_rr_nodes;++inode)
	{
		int ifanout, cut_counter, cut_pos, node_to_check;
		bool crossing_using_interposer_node = false;
		node = &rr_node[inode];
		for(ifanout=0; ifanout < node->num_edges; ++ifanout)
		{
			fanout_node = &rr_node[node->edges[ifanout]];
			for(cut_counter=0; cut_counter<num_cuts; ++cut_counter)
			{
				cut_pos = y_cuts[cut_counter];
				node_to_check = -1;
				crossing_using_interposer_node = false;

				if(node->yhigh <= cut_pos && fanout_node->ylow > cut_pos)
				{
					// this is a connection that crosses the interposer
					// make sure 'node' is an interposer node
					node_to_check = inode;
				}
				else if(node->ylow > cut_pos && fanout_node->yhigh <= cut_pos)
				{
					// this is a connection that crosses the interposer
					// make sure 'fanout_node' is an interposer node
					node_to_check = node->edges[ifanout];
				}

				if(node_to_check!=-1)
				{
					// make sure node_to_check is an interposer node
					for(i=0; i<num_interposer_nodes; ++i)
					{
						if(interposer_nodes[i]==node_to_check)
						{
							crossing_using_interposer_node = true;
							break;
						}
					}

					if(!crossing_using_interposer_node)
					{
						vpr_printf(TIO_MESSAGE_ERROR,
								"Found a connection that crosses a cut without going through an interposer node!\n");
						exit(1);
					}
				}
			}
		}
	}
	//############# END:   LEGALITY CHECK     ##########################################################
	
	//############# BEGIN: LEGALITY CHECK     ##########################################################
	// for every interposer node,
	// if INC: all fanin nodes should be below the cut, and all fanout nodes should be above the cut
	// if DEC: all fanin nodes should be above the cut, and all fanout nodes should be below the cut
	for(i=0; i<num_interposer_nodes; ++i)
	{
		int interposer_node_index = interposer_nodes[i];

		// all fanouts
		for(j=0; j< rr_node[interposer_node_index].num_edges; ++j)
		{
			int cut_pos = rr_node[interposer_node_index].ylow;
			int fanout_node_index = rr_node[interposer_node_index].edges[j];
			if(rr_node[interposer_node_index].direction==INC_DIRECTION)
			{
				assert(rr_node[fanout_node_index].ylow > cut_pos);
			}
			else if(rr_node[interposer_node_index].direction==DEC_DIRECTION)
			{
				assert(rr_node[fanout_node_index].yhigh <= cut_pos);
			}
		}

		// all fanins
		for(j=0; j< rr_node[interposer_node_index].fan_in; ++j)
		{
			int cut_pos = rr_node[interposer_node_index].ylow;
			int fanin_node_index = reverse_map[interposer_node_index][j];
			if(rr_node[interposer_node_index].direction==INC_DIRECTION)
			{
				assert(rr_node[fanin_node_index].yhigh <= cut_pos);
			}
			else if(rr_node[interposer_node_index].direction==DEC_DIRECTION)
			{
				assert(rr_node[fanin_node_index].ylow > cut_pos);
			}
		}
	}
	//############# END:   LEGALITY CHECK     ##########################################################
}









































/*

		// ######## Begin: move around Fanouts of original_node and new_node ################
		// first, let's find the size of the new fanout arrays
		int num_edges_of_original_node_after_transformations = 0;
		int num_edges_of_new_node_after_transformations = 0;
		for(cnt=0; cnt<original_node->num_edges; ++cnt)
		{
			int dnode = original_node->edges[cnt];

			if(original_node->direction==INC_DIRECTION)
			{
				if(rr_node[dnode].ylow <= cut_pos)
				{
					// keep it as a fanout of original_node
					num_edges_of_original_node_after_transformations++;
				}
				else
				{
					// this should be removed from fanout set of original_node 
					// and should be added to fanouts of the new_node
					num_edges_of_new_node_after_transformations++;
				}
			}
			else if(original_node->direction==DEC_DIRECTION)
			{
				if(rr_node[dnode].ylow > cut_pos)
				{
					// keep it as a fanout of original_node
					num_edges_of_original_node_after_transformations++;
				}
				else
				{
					// this should be removed from fanout set of original_node 
					// and should be added to fanouts of the new_node
					num_edges_of_new_node_after_transformations++;
				}
			}
		}
		
		// have to do +1 for original node, because in addition to keeping some of its old fanouts,
		// it also has 1 extra fanout which is the new_node
		num_edges_of_original_node_after_transformations += 1;

		int *temp_org_node_edges = (int*) malloc(num_edges_of_original_node_after_transformations * sizeof(int));
		int *temp_new_node_edges = (int*) malloc(num_edges_of_new_node_after_transformations * sizeof(int));
		short *temp_org_node_switches = (short*) malloc(num_edges_of_original_node_after_transformations * sizeof(short));
		short *temp_new_node_switches = (short*) malloc(num_edges_of_new_node_after_transformations * sizeof(short));
		int temp_edges_index_org =0;
		int temp_edges_index_new =0;
		for(cnt=0; cnt<original_node->num_edges; ++cnt)
		{
			int dnode = original_node->edges[cnt];
			short iswitch = original_node->switches[cnt];

			if(original_node->direction==INC_DIRECTION)
			{
				if(rr_node[dnode].ylow <= cut_pos)
				{
					// keep it as a fanout of original_node
					temp_org_node_edges[temp_edges_index_org] = dnode;
					temp_org_node_switches[temp_edges_index_org] = iswitch;
					temp_edges_index_org++;
				}
				else
				{
					// this should be removed from fanout set of original_node 
					// and should be added to fanouts of the new_node
					temp_new_node_edges[temp_edges_index_new] = dnode;
					temp_new_node_switches[temp_edges_index_new] = iswitch;
					temp_edges_index_new++;
				}
			}
			else if(original_node->direction==DEC_DIRECTION)
			{
				if(rr_node[dnode].ylow > cut_pos)
				{
					// keep it as a fanout of original_node
					temp_org_node_edges[temp_edges_index_org] = dnode;
					temp_org_node_switches[temp_edges_index_org] = iswitch;
					temp_edges_index_org++;
				}
				else
				{
					// this should be removed from fanout set of original_node 
					// and should be added to fanouts of the new_node
					temp_new_node_edges[temp_edges_index_new] = dnode;
					temp_new_node_switches[temp_edges_index_new] = iswitch;
					temp_edges_index_new++;
				}
			}
		}

		{
			// also manually add the last fanout of the original_node (edge to new_node)
			temp_org_node_edges[temp_edges_index_org] = new_node_index;
			temp_org_node_switches[temp_edges_index_org] = correct_index_of_CHANY_to_CHANY_switch;
			temp_edges_index_org++;
		}
		
		assert(temp_edges_index_org == num_edges_of_original_node_after_transformations);
		assert(temp_edges_index_new == num_edges_of_new_node_after_transformations);
		assert( (num_edges_of_original_node_after_transformations + num_edges_of_new_node_after_transformations) == 
				(original_node->num_edges + 1)
			);
		
		free(original_node->edges);
		free(original_node->switches);
		
		original_node->edges = temp_org_node_edges;
		original_node->switches = temp_org_node_switches;
		original_node->num_edges = num_edges_of_original_node_after_transformations;

		new_node->edges = temp_new_node_edges;
		new_node->switches = temp_new_node_switches;
		new_node->num_edges = num_edges_of_new_node_after_transformations;
		
		// ######## End: move around Fanouts of original_node and new_node ################

		// ######## Begin: move around Fan-Ins of original_node and new_node ################
		
		// so far, only 1 fanin has been added for new_node (connection from original_node to new_node)
		new_node->fan_in = 1;

		int num_fanins_removed_from_original_node = 0;
		for(cnt=0; cnt < original_node->fan_in; ++cnt)
		{
			// for every fan-in of the current original_node

			int fanin_node_index = reverse_map[original_node_index][cnt];
			t_rr_node* fanin_node = &rr_node[fanin_node_index];

			if(	(original_node->direction==INC_DIRECTION && fanin_node->yhigh <= cut_pos) ||
				(original_node->direction==DEC_DIRECTION && fanin_node->ylow  > cut_pos ))
			{	
				// the fanin is feeding the original_node on the same side of the cut.
				// this fanin should remain untouched.
			}
			else
			{
				// fanin_node should be removed from original_node fanin set
				// and should now feed the new_node instead of the original_node
				num_fanins_removed_from_original_node++;
				new_node->fan_in++;

				int itemp;
				for(itemp=0; itemp<fanin_node->num_edges; ++itemp)
				{
					if(fanin_node->edges[itemp] == original_node_index)
					{
						fanin_node->edges[itemp] = new_node_index;
						// not going to change the switch type
					}
				}

				// also fix the reverse_map: put a tombstone for now
				reverse_map[original_node_index][cnt] = -1;
			}
		}
		
		if(num_fanins_removed_from_original_node > 0)
		{
			// fix the reverse_map for original_node
			// the reverse_map does not include the new_nodes that are added.
			int*    original_node_fanins_after_transformations = (int*) my_malloc((original_node->fan_in - num_fanins_removed_from_original_node) * sizeof(int));
			int num_original_node_fanins_after_transformations = 0;

			for(cnt=0; cnt<original_node->fan_in; ++cnt)
			{
				int fanin_node_id = reverse_map[original_node_index][cnt];
				if(fanin_node_id != -1)
				{
					original_node_fanins_after_transformations[num_original_node_fanins_after_transformations]=fanin_node_id;
					num_original_node_fanins_after_transformations++;
				}
			}
			free(reverse_map[original_node_index]);
			reverse_map[original_node_index] = original_node_fanins_after_transformations;

			assert(num_original_node_fanins_after_transformations == (original_node->fan_in - num_fanins_removed_from_original_node));

			// LASTLY, update the fanin count of the original_node
			original_node->fan_in -= num_fanins_removed_from_original_node;
		}

		*/