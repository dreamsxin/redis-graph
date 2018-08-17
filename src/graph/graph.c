/*
* Copyright 2018-2019 Redis Labs Ltd. and Contributors
*
* This file is available under the Apache License, Version 2.0,
* modified with the Commons Clause restriction.
*/

#include "graph.h"
#include "graph_type.h"
#include "assert.h"
#include "../arithmetic/tuples_iter.h"

#define MAX(a, b) \
    ((a > b) ? a : b)

// Computes the number of blocks required to accommodate n nodes.
#define GRAPH_NODE_COUNT_TO_BLOCK_COUNT(n) \
    MAX(1, n / NODEBLOCK_CAP)

// Computes block index for given node id.
#define GRAPH_NODE_ID_TO_BLOCK_INDEX(id) \
    (id / NODEBLOCK_CAP)

// Computes node position within a block.
#define GRAPH_NODE_POSITION_WITHIN_BLOCK(id) \
    (id % NODEBLOCK_CAP)

// Get currently active block.
#define GRAPH_ACTIVE_BLOCK(g) \
    g->nodes_blocks[GRAPH_NODE_ID_TO_BLOCK_INDEX(g->node_count)]

// Retrieves block in which node id resides.
#define GRAPH_GET_NODE_BLOCK(g, id) \
    g->nodes_blocks[GRAPH_NODE_ID_TO_BLOCK_INDEX(id)]

/*========================= Graph utility functions ========================= */

/* Acquire mutex. */
void _Graph_EnterCriticalSection(Graph *g) {
    pthread_mutex_lock(&g->_mutex);
}

/* Release mutex. */
void _Graph_LeaveCriticalSection(Graph *g) {
    pthread_mutex_unlock(&g->_mutex);
}

// Resize given matrix to match graph's adjacency matrix dimensions.
void _Graph_ResizeMatrix(const Graph *g, GrB_Matrix m) {
    GrB_Index n_rows;

    GrB_Matrix_nrows(&n_rows, m);
    if(n_rows != g->node_count) {
        _Graph_EnterCriticalSection((Graph *)g);
        {
            // Double check now that we're in critical section.
            GrB_Matrix_nrows(&n_rows, m);
            if(n_rows != g->node_count)
                assert(GxB_Matrix_resize(m, g->node_count, g->node_count) == GrB_SUCCESS);
        }
        _Graph_LeaveCriticalSection((Graph *)g);
    }

}

// Resize graph's node array to contain at least n nodes.
void _Graph_ResizeNodes(Graph *g, size_t n) {
    int total_nodes = g->node_count + n;

    // Make sure we have room to store nodes.
    if (total_nodes < g->node_cap)
        return;

    int last_block = g->block_count - 1;

    // Increase NodeBlock count by the smallest multiple required to contain all nodes
    int increase_factor = (total_nodes / g->node_cap) + 2;
    g->block_count *= increase_factor;

    g->nodes_blocks = realloc(g->nodes_blocks, sizeof(NodeBlock*) * g->block_count);
    // Create and link blocks.
    for(int i = last_block; i < g->block_count-1; i++) {
        NodeBlock *block = g->nodes_blocks[i];
        NodeBlock *next_block = NodeBlock_New();
        g->nodes_blocks[i+1] = next_block;
        block->next = next_block;
    }

    g->node_cap = g->block_count * NODEBLOCK_CAP;
}

/* Relocate src node to dest node, overriding dest. */
void _Graph_NodeBlockMigrateNode(Graph *g, int src, int dest) {
    // Get the block in which dest node resides.
    NodeBlock *destNodeBlock = GRAPH_GET_NODE_BLOCK(g, dest);

    // Get node position within its block.
    int destNodeBlockIdx = GRAPH_NODE_POSITION_WITHIN_BLOCK(dest);

    // Get the src node in the graph.
    Node *srcNode = Graph_GetNode(g, src);

    srcNode->id = dest;
    // Replace dest node with src node.
    destNodeBlock->nodes[destNodeBlockIdx] = *srcNode;
}

/* Relocate src row and column, overriding dest. */
void _Graph_MigrateRowCol(Graph *g, int src, int dest) {
    GrB_Vector row;
    GrB_Vector col;
    GrB_Vector zero;
    GrB_Descriptor desc;
    GrB_Index nrows = g->node_count;
    GrB_Matrix M = Graph_GetAdjacencyMatrix(g);

    GrB_Descriptor_new(&desc);
    GrB_Descriptor_set(desc, GrB_INP0, GrB_TRAN);
    // GrB_Descriptor_set(desc, GrB_OUTP, GrB_REPLACE);

    GrB_Vector_new(&row, GrB_BOOL, nrows);
    GrB_Vector_new(&col, GrB_BOOL, nrows);
    GrB_Vector_new(&zero, GrB_BOOL, nrows);

    // Clear dest column.
    GrB_Col_assign(M, NULL, NULL, zero, GrB_ALL, nrows, dest, NULL);

    // Migrate row.
    GrB_Col_extract(row, NULL, NULL, M, GrB_ALL, nrows, src, desc);
    GrB_Row_assign(M, NULL, NULL, row, dest, GrB_ALL, nrows, NULL);

    // Migrate column.
    GrB_Col_extract(col, NULL, NULL, M, GrB_ALL, nrows, src, NULL);
    GrB_Col_assign(M, NULL, NULL, col, GrB_ALL, nrows, dest, NULL);

    for(int i = 0; i < g->relation_count; i++) {
        M = Graph_GetRelationMatrix(g, i);

        // Clear dest column.
        GrB_Col_assign(M, NULL, NULL, zero, GrB_ALL, nrows, dest, NULL);

        // Migrate row.
        GrB_Col_extract(row, NULL, NULL, M, GrB_ALL, nrows, src, desc);
        GrB_Row_assign(M, NULL, NULL, row, dest, GrB_ALL, nrows, NULL);

        // Migrate column.
        GrB_Col_extract(col, NULL, NULL, M, GrB_ALL, nrows, src, NULL);
        GrB_Col_assign(M, NULL, NULL, col, GrB_ALL, nrows, dest, NULL);
    }

    // Clean up
    GrB_Vector_free(&row);
    GrB_Vector_free(&col);
    GrB_Vector_free(&zero);
    GrB_Descriptor_free(&desc);
}

/* Removes a single entry from given matrix. */
void _Graph_ClearMatrixEntry(Graph *g, GrB_Matrix M, GrB_Index src, GrB_Index dest) {
    GrB_Vector mask;
    GrB_Index nrows = g->node_count;
    GrB_Vector_new(&mask, GrB_BOOL, nrows);
    GrB_Vector_setElement_BOOL(mask, true, dest);

    GrB_Vector col;
    GrB_Vector_new(&col, GrB_BOOL, nrows);

    GrB_Descriptor desc;
    GrB_Descriptor_new(&desc);
    GrB_Descriptor_set(desc, GrB_OUTP, GrB_REPLACE);
    GrB_Descriptor_set(desc, GrB_MASK, GrB_SCMP);

    // Extract column src_id.
    GrB_Col_extract(col, mask, NULL, M, GrB_ALL, nrows, src, desc);
    GrB_Col_assign(M, NULL, NULL, col, GrB_ALL, nrows, src, NULL);

    GrB_Descriptor_free(&desc);
    GrB_Vector_free(&col);
    GrB_Vector_free(&mask);
}

/* Deletes all edges connecting source to destination. */
void _Graph_DeleteEdges(Graph *g, NodeID src_id, NodeID dest_id) {
    GrB_Matrix M = Graph_GetAdjacencyMatrix(g);
    _Graph_ClearMatrixEntry(g, M, src_id, dest_id);

    // Update relation matrices.
    for(int i = 0; i < g->relation_count; i++) {
        M = Graph_GetRelationMatrix(g, i);
        bool connected = false;
        GrB_Matrix_extractElement_BOOL(&connected, M, dest_id, src_id);
        if(connected)
            _Graph_ClearMatrixEntry(g, M, src_id, dest_id);
    }
}

/* Deletes typed edge connecting source to destination. */
void _Graph_DeleteTypedEdges(Graph *g, NodeID src_id, NodeID dest_id, int relation) {
    bool connected = false;
    GrB_Matrix M = Graph_GetRelationMatrix(g, relation);
    assert(M);

    GrB_Matrix_extractElement_BOOL(&connected, M, dest_id, src_id);
    if(!connected) return;

    _Graph_ClearMatrixEntry(g, M, src_id, dest_id);

    // See if source is connected to destination with additional edges.
    for(int i = 0; i < g->relation_count; i++) {
        M = Graph_GetRelationMatrix(g, i);
        connected = false;
        GrB_Matrix_extractElement_BOOL(&connected, M, dest_id, src_id);
        if(connected) break;
    }

    /* There are no additional edges connecting source to destination
     * Remove edge from THE adjacency matrix. */
    if(!connected) {
        M = Graph_GetAdjacencyMatrix(g);
        _Graph_ClearMatrixEntry(g, M, src_id, dest_id);
    }
}

/*================================ Graph API ================================ */
Graph *Graph_New(size_t n) {
    assert(n > 0);
    Graph *g = malloc(sizeof(Graph));

    g->node_cap = GRAPH_NODE_COUNT_TO_BLOCK_COUNT(n) * NODEBLOCK_CAP;
    g->node_count = 0;
    g->relation_cap = GRAPH_DEFAULT_RELATION_CAP;
    g->relation_count = 0;
    g->label_cap = GRAPH_DEFAULT_LABEL_CAP;
    g->label_count = 0;

    g->block_count = GRAPH_NODE_COUNT_TO_BLOCK_COUNT(n);
    g->nodes_blocks = malloc(sizeof(NodeBlock*) * g->block_count);

    // Allocates blocks.
    for(int i = 0; i < g->block_count; i++) {
        g->nodes_blocks[i] = NodeBlock_New();
        if(i > 0) {
            // Link blocks.
            g->nodes_blocks[i-1]->next = g->nodes_blocks[i];
        }
    }

    g->_relations = malloc(sizeof(GrB_Matrix) * g->relation_cap);
    g->_labels = malloc(sizeof(GrB_Matrix) * g->label_cap);
    GrB_Matrix_new(&g->adjacency_matrix, GrB_BOOL, g->node_cap, g->node_cap);

    /* TODO: We might want a mutex per matrix,
     * such that when a thread is resizing matrix A
     * another thread could be resizing matrix B. */
    assert(pthread_mutex_init(&g->_mutex, NULL) == 0);

    return g;
}

Graph *Graph_Get(RedisModuleCtx *ctx, RedisModuleString *graph_name) {
    Graph *g = NULL;

    RedisModuleKey *key = RedisModule_OpenKey(ctx, graph_name, REDISMODULE_WRITE);
	if (RedisModule_ModuleTypeGetType(key) == GraphRedisModuleType) {
        g = RedisModule_ModuleTypeGetValue(key);
	}

    RedisModule_CloseKey(key);
    return g;
}

size_t Graph_NodeCount(const Graph *g) {
    assert(g);
    return g->node_count;
}

void Graph_CreateNodes(Graph* g, size_t n, int* labels, NodeIterator **it) {
    assert(g);

    _Graph_ResizeNodes(g, n);

    if(it != NULL) {
        *it = NodeIterator_New(GRAPH_ACTIVE_BLOCK(g),
                               g->node_count,
                               g->node_count + n,
                               1);
    }

    int node_id = g->node_count;
    g->node_count += n;

    _Graph_ResizeMatrix(g, g->adjacency_matrix);

    if(labels) {
        for(int idx = 0; idx < n; idx++) {
            int l = labels[idx];
            if(l != GRAPH_NO_LABEL) {
                GrB_Matrix m = Graph_GetLabelMatrix(g, l);
                GrB_Matrix_setElement_BOOL(m, true, node_id, node_id);
            }
            node_id++;
        }
    }
}

void Graph_ConnectNodes(Graph *g, size_t n, GrB_Index *connections) {
    assert(g && connections);
    GrB_Matrix adj = Graph_GetAdjacencyMatrix(g);
    // Update graph's adjacency matrices, setting mat[dest,src] to 1.
    for(int i = 0; i < n; i+=3) {
        int src_id = connections[i];
        int dest_id = connections[i+1];
        int r = connections[i+2];

        // Columns represent source nodes, rows represent destination nodes.
        GrB_Matrix_setElement_BOOL(adj, true, dest_id, src_id);

        if(r != GRAPH_NO_RELATION) {
            // Typed edge.
            GrB_Matrix M = Graph_GetRelationMatrix(g, r);
            GrB_Matrix_setElement_BOOL(M, true, dest_id, src_id);
        }
    }
}

Node* Graph_GetNode(const Graph *g, NodeID id) {
    assert(g && id >= 0 && id < g->node_count);

    int block_id = GRAPH_NODE_ID_TO_BLOCK_INDEX(id);

    // Make sure block_id is within range.
    if(block_id >= g->block_count) {
        return NULL;
    }

    NodeBlock *block = g->nodes_blocks[block_id];
    Node *n = &block->nodes[id%NODEBLOCK_CAP];
    n->id = id;
    return n;
}

void _replace_deleted_node(Graph *g, GrB_Vector zero, NodeID replacement, NodeID to_delete) {
    // Update label matrices.
    for (int i = 0; i < g->label_count; i ++) {
        bool src_has_label = false;
        bool dest_has_label = false;
        GrB_Matrix M = Graph_GetLabelMatrix(g, i);
        GrB_Matrix_extractElement_BOOL(&src_has_label, M, replacement, replacement);
        GrB_Matrix_extractElement_BOOL(&dest_has_label, M, to_delete, to_delete);

        if (dest_has_label && !src_has_label) {
            // Zero out the destination column if the deleted node possesses the label and the replacement does not
            assert(GrB_Col_assign(M, NULL, NULL, zero, GrB_ALL, Graph_NodeCount(g), to_delete, NULL) == GrB_SUCCESS);
        } else if (!dest_has_label && src_has_label) {
            // Set the destination column if the replacement possesses the label and the destination does not
            GrB_Matrix_setElement_BOOL(M, true, to_delete, to_delete);
        }
    }

    _Graph_MigrateRowCol(g, replacement, to_delete);
    _Graph_NodeBlockMigrateNode(g, replacement, to_delete);
}

/* Accepts a *sorted* array of IDs for nodes to be deleted.
 * The deletion is performed by swapping higher-ID nodes not scheduled
 * for deletion into lower vacant positions, until all IDs greater than
 * the updated node count are scheduled for deletion. The adjacency matrix
 * is then resized to remove these. */
void Graph_DeleteNodes(Graph *g, NodeID *IDs, size_t IDCount) {
    assert(g && IDs);
    if(IDCount == 0) return;

    int post_delete_count = g->node_count - IDCount;

    // Track the highest remaining ID in the graph
    NodeID id_to_save = g->node_count - 1;

    // Track the highest ID scheduled for deletion that is less than id_to_save
    int largest_delete_idx = IDCount - 1;
    NodeID largest_delete = IDs[largest_delete_idx];

    GrB_Vector zero;
    GrB_Vector_new(&zero, GrB_BOOL, Graph_NodeCount(g));

    // Track the lowest ID scheduled for deletion as the destination slot for
    // id_to_save
    int id_to_replace_idx = 0;
    NodeID id_to_replace;

    while ((id_to_replace = IDs[id_to_replace_idx]) < post_delete_count) {
        // Ensure that the node being saved is not scheduled for deletion
        while (id_to_save == largest_delete) {
            id_to_save --;
            largest_delete = IDs[--largest_delete_idx];
        }

        // Perform all necessary substitutions in node storage and
        // adjacency and label matrices
        _replace_deleted_node(g, zero, id_to_save, id_to_replace);

        id_to_replace_idx ++;
        if (id_to_replace_idx >= IDCount) break;
        id_to_save --;
    }

    g->node_count = post_delete_count;

    // Force matrix resizing.
    _Graph_ResizeMatrix(g, g->adjacency_matrix);
}

void Graph_DeleteEdge(Graph *g, NodeID src_id, NodeID dest_id, int relation) {
    assert(src_id < g->node_count && dest_id < g->node_count);

    // See if there's an edge between src and dest.
    bool connected = false;
    GrB_Matrix M = Graph_GetAdjacencyMatrix(g);
    GrB_Matrix_extractElement_BOOL(&connected, M, dest_id, src_id);

    if(!connected) return;

    if(relation == GRAPH_NO_RELATION) {
        // Remove every edge connecting source to destination.
        _Graph_DeleteEdges(g, src_id, dest_id);
    } else {
        // Remove typed edge connecting source to destination.
        _Graph_DeleteTypedEdges(g, src_id, dest_id, relation);
    }
}

void Graph_LabelNodes(Graph *g, NodeID start_node_id, NodeID end_node_id, int label, NodeIterator **it) {
    assert(g &&
           start_node_id < g->node_count &&
           start_node_id >= 0 &&
           start_node_id <= end_node_id &&
           end_node_id < g->node_count);

    GrB_Matrix m = Graph_GetLabelMatrix(g, label);
    for(int node_id = start_node_id; node_id <= end_node_id; node_id++) {
        GrB_Matrix_setElement_BOOL(m, true, node_id, node_id);
    }

    if(it) {
        *it = NodeIterator_New(GRAPH_GET_NODE_BLOCK(g, start_node_id),
                               start_node_id,
                               end_node_id+1,
                               1);
    }
}

NodeIterator *Graph_ScanNodes(const Graph *g) {
    assert(g);
    return NodeIterator_New(g->nodes_blocks[0], 0, g->node_count, 1);
}

int Graph_AddLabelMatrix(Graph *g) {
    assert(g);

    // Make sure we've got room for a new label matrix.
    if(g->label_count == g->label_cap) {
        g->label_cap += 4;   // allocate room for 4 new matrices.
        g->_labels = realloc(g->_labels, g->label_cap * sizeof(GrB_Matrix));
    }

    GrB_Matrix_new(&g->_labels[g->label_count++], GrB_BOOL, g->node_cap, g->node_cap);
    return g->label_count-1;
}

GrB_Matrix Graph_GetAdjacencyMatrix(const Graph *g) {
    assert(g);
    GrB_Matrix m = g->adjacency_matrix;
    _Graph_ResizeMatrix(g, m);
    return m;
}

GrB_Matrix Graph_GetLabelMatrix(const Graph *g, int label_idx) {
    assert(g && label_idx < g->label_count);
    GrB_Matrix m = g->_labels[label_idx];
    _Graph_ResizeMatrix(g, m);
    return m;
}

GrB_Matrix Graph_GetRelationMatrix(const Graph *g, int relation_idx) {
    assert(g && relation_idx < g->relation_count);
    GrB_Matrix m = g->_relations[relation_idx];
    _Graph_ResizeMatrix(g, m);
    return m;
}

int Graph_AddRelationMatrix(Graph *g) {
    assert(g);

    // Make sure we've got room for a new relation matrix.
    if(g->relation_count == g->relation_cap) {
        g->relation_cap += 4;   // allocate room for 4 new matrices.
        g->_relations = realloc(g->_relations, g->relation_cap * sizeof(GrB_Matrix));
    }

    GrB_Matrix_new(&g->_relations[g->relation_count++], GrB_BOOL, g->node_cap, g->node_cap);
    return g->relation_count-1;
}

void Graph_CommitPendingOps(Graph *g) {
    /* GraphBLAS might delay execution of operations to a later stage,
     * here we're forcing GraphBLAS to execute all of its pending operations
     * by asking for the number of entries in each matrix. */

    GrB_Matrix M;
    GrB_Index nvals;

    M = Graph_GetAdjacencyMatrix(g);
    GrB_Matrix_nvals(&nvals, M);

    for(int i = 0; i < g->relation_count; i++) {
        M = Graph_GetRelationMatrix(g, i);
        GrB_Matrix_nvals(&nvals, M);
    }

    for(int i = 0; i < g->label_count; i++) {
        M = Graph_GetLabelMatrix(g, i);
        GrB_Matrix_nvals(&nvals, M);
    }
}

void Graph_Free(Graph *g) {
    assert(g);

    /* TODO: Free nodes, currently we can't free nodes
     * as they are embedded within the chain block, as a result
     * we can't call free on a single node.
     * on the other hand when freeing a query graph, we're able
     * to call free on node. this will be resolved once we'll
     * introduce property stores. */

    // Free each node.
    // Node *node;
    // NodeIterator *it = Graph_ScanNodes(g);
    // while((node = NodeIterator_Next(it)) != NULL) {
    //     Node_Free(node);
    // }
    // NodeIterator_Free(it);

    // Free node blocks.
    for(int i = 0; i<g->block_count; i++) {
        NodeBlock_Free(g->nodes_blocks[i]);
    }
    free(g->nodes_blocks);

    // Free matrices.
    GrB_Matrix m;
    m = Graph_GetAdjacencyMatrix(g);

    GrB_Matrix_free(&m);
    for(int i = 0; i < g->relation_count; i++) {
        m = Graph_GetRelationMatrix(g, i);
        GrB_Matrix_free(&m);
    }
    free(g->_relations);

    // Free matrices.
    for(int i = 0; i < g->label_count; i++) {
        m = Graph_GetLabelMatrix(g, i);
        GrB_Matrix_free(&m);
    }
    free(g->_labels);

    pthread_mutex_destroy(&g->_mutex);
    free(g);
}
