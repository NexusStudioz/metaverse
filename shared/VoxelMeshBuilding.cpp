/*=====================================================================
VoxelMeshBuilding.cpp
---------------------
Copyright Glare Technologies Limited 2020 -
=====================================================================*/
#include "VoxelMeshBuilding.h"


#include "../shared/WorldObject.h"
#include "../dll/include/IndigoException.h"
#include "../dll/IndigoStringUtils.h"
#include "../utils/ShouldCancelCallback.h"
#include "../utils/FileUtils.h"
#include "../utils/Exception.h"
#include "../utils/PlatformUtils.h"
#include "../utils/StringUtils.h"
#include "../utils/ConPrint.h"
#include "../utils/StandardPrintOutput.h"
#include "../utils/HashMapInsertOnly2.h"
#include "../utils/Sort.h"
#include "../utils/Array2D.h"
#include "../utils/Array3D.h"
#include <limits>


class VoxelHashFunc
{
public:
	size_t operator() (const Vec3<int>& v) const
	{
		return hashBytes((const uint8*)&v.x, sizeof(int)*3); // TODO: use better hash func.
	}
};


class Vec3fHashFunc
{
public:
	size_t operator() (const Indigo::Vec3f& v) const
	{
		return hashBytes((const uint8*)&v.x, sizeof(Indigo::Vec3f)); // TODO: use better hash func.
	}
};


struct VoxelBounds
{
	Vec3<int> min;
	Vec3<int> max;
};


// Does greedy meshing.
// Splats voxels to 3d array.
static Reference<Indigo::Mesh> doMakeIndigoMeshForVoxelGroupWith3dArray(const js::Vector<Voxel, 16>& voxels, int subsample_factor)
{
	try
	{
		if(voxels.empty())
			throw glare::Exception("No voxels");

		Reference<Indigo::Mesh> mesh = new Indigo::Mesh();

		const Indigo::Vec3f vertpos_empty_key(std::numeric_limits<float>::max());
		HashMapInsertOnly2<Indigo::Vec3f, int, Vec3fHashFunc> vertpos_hash(/*empty key=*/vertpos_empty_key, /*expected_num_items=*/voxels.size());

		mesh->vert_positions.reserve(voxels.size());
		mesh->triangles.reserve(voxels.size());

		mesh->setMaxNumTexcoordSets(0);

		// Do a pass over the voxels to get the bounds
		Vec4i bounds_min(std::numeric_limits<int>::max());
		Vec4i bounds_max(std::numeric_limits<int>::min());
		for(size_t i=0; i<voxels.size(); ++i)
		{
			const Vec4i vox_pos = Vec4i(voxels[i].pos.x / subsample_factor, voxels[i].pos.y / subsample_factor, voxels[i].pos.z / subsample_factor, 0);
			bounds_min = min(bounds_min, vox_pos);
			bounds_max = max(bounds_max, vox_pos);
		}
	
		VoxelBounds bounds;
		bounds.min = Vec3<int>(bounds_min[0], bounds_min[1], bounds_min[2]);
		bounds.max = Vec3<int>(bounds_max[0], bounds_max[1], bounds_max[2]);

		// Limit voxel coordinates to something reasonable.  Also avoids integer overflows in the res computation below.
		const int min_coord = -1000000;
		const int max_coord =  1000000;
		if(bounds_min[0] < min_coord || bounds_min[1] < min_coord || bounds_min[2] < min_coord)
			throw glare::Exception("Invalid voxel position coord: " + bounds_min.toString());
		if(bounds_max[0] > max_coord || bounds_max[1] > max_coord || bounds_max[2] > max_coord)
			throw glare::Exception("Invalid voxel position coord: " + bounds_max.toString());
	
		// Do a pass over the voxels to splat into a 3d array
		const Vec3<int> res = bounds.max - bounds.min + Vec3<int>(1); // Voxel array resolution

		const int max_dim_w = 100000;
		if(res.x > max_dim_w || res.y > max_dim_w || res.z > max_dim_w)
			throw glare::Exception("Voxel dimension span exceeds " + toString(max_dim_w));

		const int64 voxel_array_size = (int64)res.x * (int64)res.y * (int64)res.z; // Use int64 to avoid overflow.
		const int64 max_voxel_array_size = (1 << 27) / sizeof(Voxel); // 128 MB
		if(voxel_array_size > max_voxel_array_size)
			throw glare::Exception("Voxel array num voxels (" + toString(voxel_array_size) + ") exceeds limit of " + toString(max_voxel_array_size));

		const Voxel empty_voxel(Vec3<int>(0), /*mat_index=*/-1);
		Array3D<Voxel> voxel_array(res.x, res.y, res.z, empty_voxel);

		for(size_t i=0; i<voxels.size(); ++i)
		{
			const Voxel& voxel = voxels[i];
			const Vec4i vox_pos = Vec4i(voxels[i].pos.x / subsample_factor, voxels[i].pos.y / subsample_factor, voxels[i].pos.z / subsample_factor, 0);
			const Vec4i indices = vox_pos - bounds_min;
			voxel_array.elem(indices[0], indices[1], indices[2]) = voxel;
		}

		//if(voxel_array.getData().size() > 100000)
		//	conPrint("voxel_array size: " + toString(voxel_array.getData().size()) + " elems, " + toString(voxel_array.getData().dataSizeBytes()) + " B");

		// For each dimension (x, y, z)
		for(int dim=0; dim<3; ++dim)
		{
			// Want the a_axis x b_axis = dim_axis
			int dim_a, dim_b;
			if(dim == 0)
			{
				dim_a = 1;
				dim_b = 2;
			}
			else if(dim == 1)
			{
				dim_a = 2;
				dim_b = 0;
			}
			else // dim == 2:
			{
				dim_a = 0;
				dim_b = 1;
			}

			// Get the extents along dim_a, dim_b
			const int a_min = bounds.min[dim_a];
			const int a_end = bounds.max[dim_a] + 1;

			const int b_min = bounds.min[dim_b];
			const int b_end = bounds.max[dim_b] + 1;

			// Walk from lower to greater coords, look for downwards facing faces
			const int dim_min = bounds.min[dim];
			const int dim_end = bounds.max[dim] + 1;

			// An array of faces that still need to be processed.  We store the face material index if the face needs to be processed, and -1 otherwise.  Processed = included in a greedy quad already.
			Array2D<int> face_needed_mat(a_end - a_min, b_end - b_min);

			for(int dim_coord = dim_min; dim_coord < dim_end; ++dim_coord)
			{
				//================= Do lower faces along dim ==========================
				// Build face_needed data for this slice
				Vec3<int> vox_pos, adjacent_vox_pos; // pos coords of current voxel, and adjacent voxel
				vox_pos[dim] = dim_coord;
				adjacent_vox_pos[dim] = dim_coord - 1;
				for(int y=b_min; y<b_end; ++y)
				for(int x=a_min; x<a_end; ++x)
				{
					vox_pos[dim_a] = x;
					vox_pos[dim_b] = y;

					int this_face_needed_mat = -1;
					const Vec3<int> vox_indices = vox_pos - bounds.min;
					const Voxel& vox = voxel_array.elem(vox_indices.x, vox_indices.y, vox_indices.z);
					if(vox.mat_index >= 0) // If there is a voxel here
					{
						adjacent_vox_pos[dim_a] = x;
						adjacent_vox_pos[dim_b] = y;
						const Vec3<int> adjacent_vox_indices = adjacent_vox_pos - bounds.min;
						if(dim_coord >= dim_min + 1) // If adjacent vox indices are in array bounds: (if dim_coord - 1 >= dim_min)
						{
							const Voxel& adjacent_vox = voxel_array.elem(adjacent_vox_indices.x, adjacent_vox_indices.y, adjacent_vox_indices.z);
							if(adjacent_vox.mat_index != vox.mat_index) // If there is no adjacent voxel, or the adjacent voxel has a different material:
								this_face_needed_mat = vox.mat_index;
						}
						else
							this_face_needed_mat = vox.mat_index;
					}
					face_needed_mat.elem(x - a_min, y - b_min) = this_face_needed_mat;
				}

				// For each voxel face:
				for(int start_y=b_min; start_y<b_end; ++start_y)
				for(int start_x=a_min; start_x<a_end; ++start_x)
				{
					const int start_face_needed_mat = face_needed_mat.elem(start_x - a_min, start_y - b_min);
					if(start_face_needed_mat >= 0) // If we need a face here:
					{
						// Start a quad here (start corner at (start_x, start_y))
						// The quad will range from (start_x, start_y) to (end_x, end_y)
						int end_x = start_x + 1;
						int end_y = start_y + 1;

						bool x_increase_ok = true;
						bool y_increase_ok = true;
						while(x_increase_ok || y_increase_ok)
						{
							// Try and increase in x direction
							if(x_increase_ok)
							{
								if(end_x < a_end) // If there is still room to increase in x direction:
								{
									// Check y values for new x = end_x
									for(int y = start_y; y < end_y; ++y)
										if(face_needed_mat.elem(end_x - a_min, y - b_min) != start_face_needed_mat)
										{
											x_increase_ok = false;
											break;
										}

									if(x_increase_ok)
										end_x++;
								}
								else
									x_increase_ok = false;
							}

							// Try and increase in y direction
							if(y_increase_ok)
							{
								if(end_y < b_end)
								{
									// Check x values for new y = end_y
									for(int x = start_x; x < end_x; ++x)
										if(face_needed_mat.elem(x - a_min, end_y - b_min) != start_face_needed_mat)
										{
											y_increase_ok = false;
											break;
										}

									if(y_increase_ok)
										end_y++;
								}
								else
									y_increase_ok = false;
							}
						}

						// We have worked out the greedy quad.  Mark elements in it as processed
						for(int y=start_y; y < end_y; ++y)
							for(int x=start_x; x < end_x; ++x)
								face_needed_mat.elem(x - a_min, y - b_min) = -1;

						// Add the greedy quad
						unsigned int v_i[4]; // quad vert indices
						Indigo::Vec3f v;
						v[dim] = (float)dim_coord;
						{
							// bot left
							v[dim_a] = (float)start_x;
							v[dim_b] = (float)start_y;

							// returns object of type std::pair<iterator, bool>
							const auto insert_res = vertpos_hash.insert(std::make_pair(v, (int)vertpos_hash.size())); // Try and insert vertex
							v_i[0] = insert_res.first->second; // Get existing or new item (insert_res.first) - a (vec3f, index) pair, then get the index.
							if(insert_res.second) // If inserted new value:
								mesh->vert_positions.push_back(v);
						}
						{
							// top left
							v[dim_a] = (float)start_x;
							v[dim_b] = (float)end_y;

							const auto insert_res = vertpos_hash.insert(std::make_pair(v, (int)vertpos_hash.size()));
							v_i[1] = insert_res.first->second; // deref iterator to get (vec3f, index) pair, then get the index.
							if(insert_res.second) // If inserted new value:
								mesh->vert_positions.push_back(v);
						}
						{
							// top right
							v[dim_a] = (float)end_x;
							v[dim_b] = (float)end_y;

							const auto insert_res = vertpos_hash.insert(std::make_pair(v, (int)vertpos_hash.size()));
							v_i[2] = insert_res.first->second; // deref iterator to get (vec3f, index) pair, then get the index.
							if(insert_res.second) // If inserted new value:
								mesh->vert_positions.push_back(v);
						}
						{
							// bot right
							v[dim_a] = (float)end_x;
							v[dim_b] = (float)start_y;

							const auto insert_res = vertpos_hash.insert(std::make_pair(v, (int)vertpos_hash.size()));
							v_i[3] = insert_res.first->second; // deref iterator to get (vec3f, index) pair, then get the index.
							if(insert_res.second) // If inserted new value:
								mesh->vert_positions.push_back(v);
						}

						assert(mesh->vert_positions.size() == vertpos_hash.size());

						const size_t tri_start = mesh->triangles.size();
						mesh->triangles.resize(tri_start + 2);

						mesh->triangles[tri_start + 0].vertex_indices[0] = v_i[0];
						mesh->triangles[tri_start + 0].vertex_indices[1] = v_i[1];
						mesh->triangles[tri_start + 0].vertex_indices[2] = v_i[2];
						mesh->triangles[tri_start + 0].uv_indices[0]     = 0;
						mesh->triangles[tri_start + 0].uv_indices[1]     = 0;
						mesh->triangles[tri_start + 0].uv_indices[2]     = 0;
						mesh->triangles[tri_start + 0].tri_mat_index     = (uint32)start_face_needed_mat;

						mesh->triangles[tri_start + 1].vertex_indices[0] = v_i[0];
						mesh->triangles[tri_start + 1].vertex_indices[1] = v_i[2];
						mesh->triangles[tri_start + 1].vertex_indices[2] = v_i[3];
						mesh->triangles[tri_start + 1].uv_indices[0]     = 0;
						mesh->triangles[tri_start + 1].uv_indices[1]     = 0;
						mesh->triangles[tri_start + 1].uv_indices[2]     = 0;
						mesh->triangles[tri_start + 1].tri_mat_index     = (uint32)start_face_needed_mat;
					}
				}

				//================= Do upper faces along dim ==========================
				// Build face_needed data for this slice
				adjacent_vox_pos[dim] = dim_coord + 1;
				for(int y=b_min; y<b_end; ++y)
				for(int x=a_min; x<a_end; ++x)
				{
					vox_pos[dim_a] = x;
					vox_pos[dim_b] = y;
					const Vec3<int> vox_indices = vox_pos - bounds.min;
					const Voxel& vox = voxel_array.elem(vox_indices.x, vox_indices.y, vox_indices.z);

					int this_face_needed_mat = -1;
					if(vox.mat_index >= 0) // If there is a voxel here
					{
						adjacent_vox_pos[dim_a] = x;
						adjacent_vox_pos[dim_b] = y;
						const Vec3<int> adjacent_vox_indices = adjacent_vox_pos - bounds.min;
						if(dim_coord < dim_end - 1)// If adjacent vox indices are in array bounds: (if dim_coord + 1 < dim_end)
						{
							const Voxel& adjacent_vox = voxel_array.elem(adjacent_vox_indices.x, adjacent_vox_indices.y, adjacent_vox_indices.z);
							if(adjacent_vox.mat_index != vox.mat_index) // If there is no adjacent voxel, or the adjacent voxel has a different material:
								this_face_needed_mat = vox.mat_index;
						}
						else
							this_face_needed_mat = vox.mat_index;
					}
					face_needed_mat.elem(x - a_min, y - b_min) = this_face_needed_mat;
				}

				// For each voxel face:
				for(int start_y=b_min; start_y<b_end; ++start_y)
				for(int start_x=a_min; start_x<a_end; ++start_x)
				{
					const int start_face_needed_mat = face_needed_mat.elem(start_x - a_min, start_y - b_min);
					if(start_face_needed_mat >= 0)
					{
						// Start a quad here (start corner at (start_x, start_y))
						// The quad will range from (start_x, start_y) to (end_x, end_y)
						int end_x = start_x + 1;
						int end_y = start_y + 1;

						bool x_increase_ok = true;
						bool y_increase_ok = true;
						while(x_increase_ok || y_increase_ok)
						{
							// Try and increase in x direction
							if(x_increase_ok)
							{
								if(end_x < a_end) // If there is still room to increase in x direction:
								{
									// Check y values for new x = end_x
									for(int y = start_y; y < end_y; ++y)
										if(face_needed_mat.elem(end_x - a_min, y - b_min) != start_face_needed_mat)
										{
											x_increase_ok = false;
											break;
										}

									if(x_increase_ok)
										end_x++;
								}
								else
									x_increase_ok = false;
							}

							// Try and increase in y direction
							if(y_increase_ok)
							{
								if(end_y < b_end)
								{
									// Check x values for new y = end_y
									for(int x = start_x; x < end_x; ++x)
										if(face_needed_mat.elem(x - a_min, end_y - b_min) != start_face_needed_mat)
										{
											y_increase_ok = false;
											break;
										}

									if(y_increase_ok)
										end_y++;
								}
								else
									y_increase_ok = false;
							}
						}

						// We have worked out the greedy quad.  Mark elements in it as processed
						for(int y=start_y; y < end_y; ++y)
							for(int x=start_x; x < end_x; ++x)
								face_needed_mat.elem(x - a_min, y - b_min) = -1;

						const float quad_dim_coord = (float)(dim_coord + 1);

						// Add the greedy quad
						unsigned int v_i[4]; // quad vert indices
						Indigo::Vec3f v;
						v[dim] = (float)quad_dim_coord;
						{ // Add bot left vert
							v[dim_a] = (float)start_x;
							v[dim_b] = (float)start_y;

							const auto insert_res = vertpos_hash.insert(std::make_pair(v, (int)vertpos_hash.size()));
							v_i[0] = insert_res.first->second; // deref iterator to get (vec3f, index) pair, then get the index.
							if(insert_res.second) // If inserted new value:
								mesh->vert_positions.push_back(v);
						}
						{ // bot right
							v[dim_a] = (float)end_x;
							v[dim_b] = (float)start_y;

							const auto insert_res = vertpos_hash.insert(std::make_pair(v, (int)vertpos_hash.size()));
							v_i[1] = insert_res.first->second; // deref iterator to get (vec3f, index) pair, then get the index.
							if(insert_res.second) // If inserted new value:
								mesh->vert_positions.push_back(v);
						}
						{ // top right
							v[dim_a] = (float)end_x;
							v[dim_b] = (float)end_y;

							const auto insert_res = vertpos_hash.insert(std::make_pair(v, (int)vertpos_hash.size()));
							v_i[2] = insert_res.first->second; // deref iterator to get (vec3f, index) pair, then get the index.
							if(insert_res.second) // If inserted new value:
								mesh->vert_positions.push_back(v);
						}
						{ // top left
							v[dim_a] = (float)start_x;
							v[dim_b] = (float)end_y;

							const auto insert_res = vertpos_hash.insert(std::make_pair(v, (int)vertpos_hash.size()));
							v_i[3] = insert_res.first->second; // deref iterator to get (vec3f, index) pair, then get the index.
							if(insert_res.second) // If inserted new value:
								mesh->vert_positions.push_back(v);
						}

						const size_t tri_start = mesh->triangles.size();
						mesh->triangles.resize(tri_start + 2);

						mesh->triangles[tri_start + 0].vertex_indices[0] = v_i[0];
						mesh->triangles[tri_start + 0].vertex_indices[1] = v_i[1];
						mesh->triangles[tri_start + 0].vertex_indices[2] = v_i[2];
						mesh->triangles[tri_start + 0].uv_indices[0]     = 0;
						mesh->triangles[tri_start + 0].uv_indices[1]     = 0;
						mesh->triangles[tri_start + 0].uv_indices[2]     = 0;
						mesh->triangles[tri_start + 0].tri_mat_index     = (uint32)start_face_needed_mat;

						mesh->triangles[tri_start + 1].vertex_indices[0] = v_i[0];
						mesh->triangles[tri_start + 1].vertex_indices[1] = v_i[2];
						mesh->triangles[tri_start + 1].vertex_indices[2] = v_i[3];
						mesh->triangles[tri_start + 1].uv_indices[0]     = 0;
						mesh->triangles[tri_start + 1].uv_indices[1]     = 0;
						mesh->triangles[tri_start + 1].uv_indices[2]     = 0;
						mesh->triangles[tri_start + 1].tri_mat_index     = (uint32)start_face_needed_mat;
					}
				}
			}
		}

		mesh->endOfModel();
		assert(isFinite(mesh->aabb_os.bound[0].x));
		return mesh;
	}
	catch(Indigo::IndigoException& e)
	{
		throw glare::Exception(toStdString(e.what()));
	}
}


struct VoxelVertInfo
{
	VoxelVertInfo() {}
	VoxelVertInfo(const Indigo::Vec3f& pos_, const Indigo::Vec3f& normal_) : pos(pos_), normal(normal_) {}

	inline bool operator == (const VoxelVertInfo& other) const { return pos == other.pos && normal == other.normal; }
	inline bool operator != (const VoxelVertInfo& other) const { return pos != other.pos || normal != other.normal; }

	Indigo::Vec3f pos;
	Indigo::Vec3f normal;
};


struct VoxelVertInfoHashFunc
{
	size_t operator() (const VoxelVertInfo& v) const
	{
		return hashBytes((const uint8*)&v.pos.x, sizeof(Indigo::Vec3f)); // TODO: use better hash func.
	}
};


#if 0
// Does greedy meshing
// Computes vertex normals, thereby avoiding reusing vertices with the same positions but different normals.
static Reference<Indigo::Mesh> doMakeIndigoMeshWithNormalsForVoxelGroup(const js::Vector<Voxel, 16>& voxels, const size_t num_mats, const HashMapInsertOnly2<Vec3<int>, int, VoxelHashFunc>& voxel_hash)
{
	Reference<Indigo::Mesh> mesh = new Indigo::Mesh();

	VoxelVertInfo vertpos_empty_key;
	vertpos_empty_key.pos = Indigo::Vec3f(std::numeric_limits<float>::max());
	vertpos_empty_key.normal = Indigo::Vec3f(std::numeric_limits<float>::max());
	HashMapInsertOnly2<VoxelVertInfo, int, VoxelVertInfoHashFunc> vertpos_hash(/*empty key=*/vertpos_empty_key, /*expected_num_items=*/voxels.size());

	mesh->vert_positions.reserve(voxels.size());
	mesh->vert_normals.reserve(voxels.size());
	mesh->triangles.reserve(voxels.size());

	mesh->setMaxNumTexcoordSets(0);

	VoxelBounds b;
	b.min = Vec3<int>( 1000000000);
	b.max = Vec3<int>(-1000000000);
	std::vector<VoxelBounds> mat_vox_bounds(num_mats, b);
	for(size_t i=0; i<voxels.size(); ++i) // For each mat
	{
		const int mat_index = voxels[i].mat_index;
		mat_vox_bounds[mat_index].min = mat_vox_bounds[mat_index].min.min(voxels[i].pos);
		mat_vox_bounds[mat_index].max = mat_vox_bounds[mat_index].max.max(voxels[i].pos);
	}

	for(size_t mat_i=0; mat_i<num_mats; ++mat_i) // For each mat
	{
		if(mat_vox_bounds[mat_i].min == Vec3<int>(1000000000))
			continue; // No voxels for this mat.

		// For each dimension (x, y, z)
		for(int dim=0; dim<3; ++dim)
		{
			// Want the a_axis x b_axis = dim_axis
			int dim_a, dim_b;
			if(dim == 0)
			{
				dim_a = 1;
				dim_b = 2;
			}
			else if(dim == 1)
			{
				dim_a = 2;
				dim_b = 0;
			}
			else // dim == 2:
			{
				dim_a = 0;
				dim_b = 1;
			}

			Indigo::Vec3f normal_up(0.f);
			normal_up[dim] = 1.f;
			Indigo::Vec3f normal_down(0.f);
			normal_down[dim] = -1.f;

			// Get the extents along dim_a, dim_b
			const int a_min = mat_vox_bounds[mat_i].min[dim_a];
			const int a_end = mat_vox_bounds[mat_i].max[dim_a] + 1;

			const int b_min = mat_vox_bounds[mat_i].min[dim_b];
			const int b_end = mat_vox_bounds[mat_i].max[dim_b] + 1;

			// Walk from lower to greater coords, look for downwards facing faces
			const int dim_min = mat_vox_bounds[mat_i].min[dim];
			const int dim_end = mat_vox_bounds[mat_i].max[dim] + 1;

			// Make an array to indicate processed voxel faces.  Processed = included in a greedy quad already.
			Array2D<bool> vox_present(a_end - a_min, b_end - b_min); // Memorize the voxel lookup to use for building upper faces
			Array2D<bool> face_needed(a_end - a_min, b_end - b_min);

			for(int dim_coord = dim_min; dim_coord < dim_end; ++dim_coord)
			{
				//================= Do lower faces along dim ==========================
				// Build face_needed data for this slice
				Vec3<int> vox, adjacent_vox_pos;
				vox[dim] = dim_coord;
				adjacent_vox_pos[dim] = dim_coord - 1;
				for(int y=b_min; y<b_end; ++y)
				for(int x=a_min; x<a_end; ++x)
				{
					vox[dim_a] = x;
					vox[dim_b] = y;

					bool this_face_needed = false;
					bool this_vox_present = false;
					auto res = voxel_hash.find(vox);
					if((res != voxel_hash.end()) && (res->second == mat_i)) // If there is a voxel here with mat_i
					{
						this_vox_present = true;

						adjacent_vox_pos[dim_a] = x;
						adjacent_vox_pos[dim_b] = y;
						auto adjacent_res = voxel_hash.find(adjacent_vox_pos);
						if((adjacent_res == voxel_hash.end()) || (adjacent_res->second != mat_i)) // If there is no adjacent voxel, or the adjacent voxel has a different material:
							this_face_needed = true;
					}
					vox_present.elem(x - a_min, y - b_min) = this_vox_present;
					face_needed.elem(x - a_min, y - b_min) = this_face_needed;
				}

				// For each voxel face:
				for(int start_y=b_min; start_y<b_end; ++start_y)
				for(int start_x=a_min; start_x<a_end; ++start_x)
				{
					if(face_needed.elem(start_x - a_min, start_y - b_min)) // If we need a face here:
					{
						// Start a quad here (start corner at (start_x, start_y))
						// The quad will range from (start_x, start_y) to (end_x, end_y)
						int end_x = start_x + 1;
						int end_y = start_y + 1;

						bool x_increase_ok = true;
						bool y_increase_ok = true;
						while(x_increase_ok || y_increase_ok)
						{
							// Try and increase in x direction
							if(x_increase_ok)
							{
								if(end_x < a_end) // If there is still room to increase in x direction:
								{
									// Check y values for new x = end_x
									for(int y = start_y; y < end_y; ++y)
										if(!face_needed.elem(end_x - a_min, y - b_min))
										{
											x_increase_ok = false;
											break;
										}

									if(x_increase_ok)
										end_x++;
								}
								else
									x_increase_ok = false;
							}

							// Try and increase in y direction
							if(y_increase_ok)
							{
								if(end_y < b_end)
								{
									// Check x values for new y = end_y
									for(int x = start_x; x < end_x; ++x)
										if(!face_needed.elem(x - a_min, end_y - b_min))
										{
											y_increase_ok = false;
											break;
										}

									if(y_increase_ok)
										end_y++;
								}
								else
									y_increase_ok = false;
							}
						}

						// We have worked out the greedy quad.  Mark elements in it as processed
						for(int y=start_y; y < end_y; ++y)
						for(int x=start_x; x < end_x; ++x)
							face_needed.elem(x - a_min, y - b_min) = false;

						// Add the greedy quad
						unsigned int v_i[4]; // quad vert indices
						Indigo::Vec3f v;
						v[dim] = (float)dim_coord;
						{
							// bot left
							v[dim_a] = (float)start_x;
							v[dim_b] = (float)start_y;

							// returns object of type std::pair<iterator, bool>
							VoxelVertInfo vert_info(v, normal_down);
							const auto insert_res = vertpos_hash.insert(std::make_pair(vert_info, (int)vertpos_hash.size())); // Try and insert vertex
							v_i[0] = insert_res.first->second; // Get existing or new item (insert_res.first) - a (vec3f, index) pair, then get the index.
							if(insert_res.second) // If inserted new value:
							{
								mesh->vert_positions.push_back(v);
								mesh->vert_normals.push_back(normal_down);
							}
						}
						{
							// top left
							v[dim_a] = (float)start_x;
							v[dim_b] = (float)end_y;

							VoxelVertInfo vert_info(v, normal_down);
							const auto insert_res = vertpos_hash.insert(std::make_pair(vert_info, (int)vertpos_hash.size()));
							v_i[1] = insert_res.first->second; // deref iterator to get (vec3f, index) pair, then get the index.
							if(insert_res.second) // If inserted new value:
							{
								mesh->vert_positions.push_back(v);
								mesh->vert_normals.push_back(normal_down);
							}
						}
						{
							// top right
							v[dim_a] = (float)end_x;
							v[dim_b] = (float)end_y;

							VoxelVertInfo vert_info(v, normal_down);
							const auto insert_res = vertpos_hash.insert(std::make_pair(vert_info, (int)vertpos_hash.size()));
							v_i[2] = insert_res.first->second; // deref iterator to get (vec3f, index) pair, then get the index.
							if(insert_res.second) // If inserted new value:
							{
								mesh->vert_positions.push_back(v);
								mesh->vert_normals.push_back(normal_down);
							}
						}
						{
							// bot right
							v[dim_a] = (float)end_x;
							v[dim_b] = (float)start_y;

							VoxelVertInfo vert_info(v, normal_down);
							const auto insert_res = vertpos_hash.insert(std::make_pair(vert_info, (int)vertpos_hash.size()));
							v_i[3] = insert_res.first->second; // deref iterator to get (vec3f, index) pair, then get the index.
							if(insert_res.second) // If inserted new value:
							{
								mesh->vert_positions.push_back(v);
								mesh->vert_normals.push_back(normal_down);
							}
						}

						assert(mesh->vert_positions.size() == vertpos_hash.size());

						const size_t tri_start = mesh->triangles.size();
						mesh->triangles.resize(tri_start + 2);
						
						mesh->triangles[tri_start + 0].vertex_indices[0] = v_i[0];
						mesh->triangles[tri_start + 0].vertex_indices[1] = v_i[1];
						mesh->triangles[tri_start + 0].vertex_indices[2] = v_i[2];
						mesh->triangles[tri_start + 0].uv_indices[0]     = 0;
						mesh->triangles[tri_start + 0].uv_indices[1]     = 0;
						mesh->triangles[tri_start + 0].uv_indices[2]     = 0;
						mesh->triangles[tri_start + 0].tri_mat_index     = (uint32)mat_i;
						
						mesh->triangles[tri_start + 1].vertex_indices[0] = v_i[0];
						mesh->triangles[tri_start + 1].vertex_indices[1] = v_i[2];
						mesh->triangles[tri_start + 1].vertex_indices[2] = v_i[3];
						mesh->triangles[tri_start + 1].uv_indices[0]     = 0;
						mesh->triangles[tri_start + 1].uv_indices[1]     = 0;
						mesh->triangles[tri_start + 1].uv_indices[2]     = 0;
						mesh->triangles[tri_start + 1].tri_mat_index     = (uint32)mat_i;
					}
				}

				//================= Do upper faces along dim ==========================
				// Build face_needed data for this slice
				adjacent_vox_pos[dim] = dim_coord + 1;
				for(int y=b_min; y<b_end; ++y)
				for(int x=a_min; x<a_end; ++x)
				{
					bool this_face_needed = false;
					if(vox_present.elem(x - a_min, y - b_min)) // If there is a voxel here with mat_i
					{
						adjacent_vox_pos[dim_a] = x;
						adjacent_vox_pos[dim_b] = y;
						auto adjacent_res = voxel_hash.find(adjacent_vox_pos);
						if((adjacent_res == voxel_hash.end()) || (adjacent_res->second != mat_i)) // If there is no adjacent voxel, or the adjacent voxel has a different material:
							this_face_needed = true;
					}
					face_needed.elem(x - a_min, y - b_min) = this_face_needed;
				}

				// For each voxel face:
				for(int start_y=b_min; start_y<b_end; ++start_y)
				for(int start_x=a_min; start_x<a_end; ++start_x)
				{
					if(face_needed.elem(start_x - a_min, start_y - b_min))
					{
						// Start a quad here (start corner at (start_x, start_y))
						// The quad will range from (start_x, start_y) to (end_x, end_y)
						int end_x = start_x + 1;
						int end_y = start_y + 1;

						bool x_increase_ok = true;
						bool y_increase_ok = true;
						while(x_increase_ok || y_increase_ok)
						{
							// Try and increase in x direction
							if(x_increase_ok)
							{
								if(end_x < a_end) // If there is still room to increase in x direction:
								{
									// Check y values for new x = end_x
									for(int y = start_y; y < end_y; ++y)
										if(!face_needed.elem(end_x - a_min, y - b_min))
										{
											x_increase_ok = false;
											break;
										}

									if(x_increase_ok)
										end_x++;
								}
								else
									x_increase_ok = false;
							}

							// Try and increase in y direction
							if(y_increase_ok)
							{
								if(end_y < b_end)
								{
									// Check x values for new y = end_y
									for(int x = start_x; x < end_x; ++x)
										if(!face_needed.elem(x - a_min, end_y - b_min))
										{
											y_increase_ok = false;
											break;
										}

									if(y_increase_ok)
										end_y++;
								}
								else
									y_increase_ok = false;
							}
						}

						// We have worked out the greedy quad.  Mark elements in it as processed
						for(int y=start_y; y < end_y; ++y)
							for(int x=start_x; x < end_x; ++x)
								face_needed.elem(x - a_min, y - b_min) = false;

						const float quad_dim_coord = (float)(dim_coord + 1);

						// Add the greedy quad
						unsigned int v_i[4]; // quad vert indices
						Indigo::Vec3f v;
						v[dim] = (float)quad_dim_coord;
						{ // Add bot left vert
							v[dim_a] = (float)start_x;
							v[dim_b] = (float)start_y;
								
							VoxelVertInfo vert_info(v, normal_up);
							const auto insert_res = vertpos_hash.insert(std::make_pair(vert_info, (int)vertpos_hash.size()));
							v_i[0] = insert_res.first->second; // deref iterator to get (vec3f, index) pair, then get the index.
							if(insert_res.second) // If inserted new value:
							{
								mesh->vert_positions.push_back(v);
								mesh->vert_normals.push_back(normal_up);
							}
						}
						{ // bot right
							v[dim_a] = (float)end_x;
							v[dim_b] = (float)start_y;

							VoxelVertInfo vert_info(v, normal_up);
							const auto insert_res = vertpos_hash.insert(std::make_pair(vert_info, (int)vertpos_hash.size()));
							v_i[1] = insert_res.first->second; // deref iterator to get (vec3f, index) pair, then get the index.
							if(insert_res.second) // If inserted new value:
							{
								mesh->vert_positions.push_back(v);
								mesh->vert_normals.push_back(normal_up);
							}
						}
						{ // top right
							v[dim_a] = (float)end_x;
							v[dim_b] = (float)end_y;

							VoxelVertInfo vert_info(v, normal_up);
							const auto insert_res = vertpos_hash.insert(std::make_pair(vert_info, (int)vertpos_hash.size()));
							v_i[2] = insert_res.first->second; // deref iterator to get (vec3f, index) pair, then get the index.
							if(insert_res.second) // If inserted new value:
							{
								mesh->vert_positions.push_back(v);
								mesh->vert_normals.push_back(normal_up);
							}
						}
						{ // top left
							v[dim_a] = (float)start_x;
							v[dim_b] = (float)end_y;

							VoxelVertInfo vert_info(v, normal_up);
							const auto insert_res = vertpos_hash.insert(std::make_pair(vert_info, (int)vertpos_hash.size()));
							v_i[3] = insert_res.first->second; // deref iterator to get (vec3f, index) pair, then get the index.
							if(insert_res.second) // If inserted new value:
							{
								mesh->vert_positions.push_back(v);
								mesh->vert_normals.push_back(normal_up);
							}
						}
							
						const size_t tri_start = mesh->triangles.size();
						mesh->triangles.resize(tri_start + 2);
							
						mesh->triangles[tri_start + 0].vertex_indices[0] = v_i[0];
						mesh->triangles[tri_start + 0].vertex_indices[1] = v_i[1];
						mesh->triangles[tri_start + 0].vertex_indices[2] = v_i[2];
						mesh->triangles[tri_start + 0].uv_indices[0]     = 0;
						mesh->triangles[tri_start + 0].uv_indices[1]     = 0;
						mesh->triangles[tri_start + 0].uv_indices[2]     = 0;
						mesh->triangles[tri_start + 0].tri_mat_index     = (uint32)mat_i;

						mesh->triangles[tri_start + 1].vertex_indices[0] = v_i[0];
						mesh->triangles[tri_start + 1].vertex_indices[1] = v_i[2];
						mesh->triangles[tri_start + 1].vertex_indices[2] = v_i[3];
						mesh->triangles[tri_start + 1].uv_indices[0]     = 0;
						mesh->triangles[tri_start + 1].uv_indices[1]     = 0;
						mesh->triangles[tri_start + 1].uv_indices[2]     = 0;
						mesh->triangles[tri_start + 1].tri_mat_index     = (uint32)mat_i;
					}
				}
			}
		}
	}

	mesh->endOfModel();
	assert(isFinite(mesh->aabb_os.bound[0].x));
	return mesh;
}
#endif


Reference<Indigo::Mesh> VoxelMeshBuilding::makeIndigoMeshForVoxelGroup(const VoxelGroup& voxel_group, const int subsample_factor, bool generate_shading_normals)
{
	assert(voxel_group.voxels.size() > 0);
	// conPrint("Adding " + toString(voxel_group.voxels.size()) + " voxels.");

	return doMakeIndigoMeshForVoxelGroupWith3dArray(voxel_group.voxels, subsample_factor);
}


#if BUILD_TESTS


#if 0
// Command line:
// C:\fuzz_corpus\voxel_data N:\new_cyberspace\trunk\testfiles\voxels

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
	try
	{
		VoxelGroup group;
		group.voxels.resize(size / sizeof(Voxel));
		std::memcpy(group.voxels.data(), data, group.voxels.dataSizeBytes());

		doMakeIndigoMeshForVoxelGroupWith3dArray(group.voxels, 1);
		doMakeIndigoMeshForVoxelGroupWith3dArray(group.voxels, 2);
		doMakeIndigoMeshForVoxelGroupWith3dArray(group.voxels, 4);
	}
	catch(glare::Exception&)
	{
	}

	return 0;  // Non-zero return values are reserved for future use.
}
#endif


#include <simpleraytracer/raymesh.h>
#include <utils/TaskManager.h>
#include <utils/TestUtils.h>


void VoxelMeshBuilding::test()
{
	conPrint("VoxelMeshBuilding::test()");

	glare::TaskManager task_manager;

	// Test a single voxel
	{
		VoxelGroup group;
		group.voxels.push_back(Voxel(Vec3<int>(0, 0, 0), 0));

		Reference<Indigo::Mesh> data = makeIndigoMeshForVoxelGroup(group, /*subsample_factor=*/1, /*generate_shading_normals=*/false);

		testAssert(data->num_materials_referenced == 1);
		testAssert(data->triangles.size() == 6 * 2);
		testAssert(data->aabb_os.bound[0] == Indigo::Vec3f(0,0,0));
		testAssert(data->aabb_os.bound[1] == Indigo::Vec3f(1,1,1));

		// Test with subsampling
		data = makeIndigoMeshForVoxelGroup(group, /*subsample_factor=*/4, /*generate_shading_normals=*/false);
		testAssert(data->num_materials_referenced == 1);
		testAssert(data->triangles.size() == 6 * 2);
		testAssert(data->aabb_os.bound[0] == Indigo::Vec3f(0,0,0));
		testAssert(data->aabb_os.bound[1] == Indigo::Vec3f(1,1,1));
	}

	{
		VoxelGroup group;
		group.voxels.push_back(Voxel(Vec3<int>(0, 0, 0), 0));
		group.voxels.push_back(Voxel(Vec3<int>(10, 0, 1), 1));
		group.voxels.push_back(Voxel(Vec3<int>(20, 0, 1), 0));
		group.voxels.push_back(Voxel(Vec3<int>(30, 0, 1), 1));
		group.voxels.push_back(Voxel(Vec3<int>(40, 0, 1), 0));
		group.voxels.push_back(Voxel(Vec3<int>(50, 0, 1), 1));

		Reference<Indigo::Mesh> data = makeIndigoMeshForVoxelGroup(group, /*subsample_factor=*/1, /*generate_shading_normals=*/false);

		testAssert(data->num_materials_referenced == 2);
		testAssert(data->triangles.size() == 6 * 6 * 2);
		testAssert(data->aabb_os.bound[0] == Indigo::Vec3f(0,0,0));
		testAssert(data->aabb_os.bound[1] == Indigo::Vec3f(51,1,2));

		// Test with subsampling
		data = makeIndigoMeshForVoxelGroup(group, /*subsample_factor=*/4, /*generate_shading_normals=*/false);
		testAssert(data->num_materials_referenced == 2);
		testAssert(data->triangles.size() == 6 * 6 * 2);
		testAssert(data->aabb_os.bound[0] == Indigo::Vec3f(0,0,0));
		testAssert(data->aabb_os.bound[1] == Indigo::Vec3f(50/4 + 1, 1, 2/4 + 1));
	}

	

	// Test two adjacent voxels with same material.  Greedy meshing should result in just 6 quad faces (12 tris)
	{
		VoxelGroup group;
		group.voxels.push_back(Voxel(Vec3<int>(0, 0, 0), 0));
		group.voxels.push_back(Voxel(Vec3<int>(1, 0, 0), 0));

		Reference<Indigo::Mesh> data = makeIndigoMeshForVoxelGroup(group, /*subsample_factor=*/1, /*generate_shading_normals=*/false);

		testAssert(data->num_materials_referenced == 1);
		testAssert(data->triangles.size() == 6 * 2);

		// Test with subsampling
		data = makeIndigoMeshForVoxelGroup(group, /*subsample_factor=*/4, /*generate_shading_normals=*/false);
		testAssert(data->num_materials_referenced == 1);
		testAssert(data->triangles.size() == 6 * 2);
	}

	// Test two adjacent voxels (along y axis) with same material.  Greedy meshing should result in just 6 quad faces (12 tris)
	{
		VoxelGroup group;
		group.voxels.push_back(Voxel(Vec3<int>(0, 0, 0), 0));
		group.voxels.push_back(Voxel(Vec3<int>(0, 1, 0), 0));


		Reference<Indigo::Mesh> data = makeIndigoMeshForVoxelGroup(group, /*subsample_factor=*/1, /*generate_shading_normals=*/false);

		testAssert(data->num_materials_referenced == 1);
		testAssert(data->triangles.size() == 6 * 2);

		// Test with subsampling
		data = makeIndigoMeshForVoxelGroup(group, /*subsample_factor=*/4, /*generate_shading_normals=*/false);
		testAssert(data->num_materials_referenced == 1);
		testAssert(data->triangles.size() == 6 * 2);
	}

	// Test two adjacent voxels (along z axis) with same material.  Greedy meshing should result in just 6 quad faces (12 tris)
	{
		VoxelGroup group;
		group.voxels.push_back(Voxel(Vec3<int>(0, 0, 0), 0));
		group.voxels.push_back(Voxel(Vec3<int>(0, 0, 1), 0));

		Reference<Indigo::Mesh> data = makeIndigoMeshForVoxelGroup(group, /*subsample_factor=*/1, /*generate_shading_normals=*/false);

		testAssert(data->num_materials_referenced == 1);
		testAssert(data->triangles.size() == 6 * 2);

		// Test with subsampling
		data = makeIndigoMeshForVoxelGroup(group, /*subsample_factor=*/2, /*generate_shading_normals=*/false);
		testAssert(data->num_materials_referenced == 1);
		testAssert(data->triangles.size() == 6 * 2);
	}

	// Test two adjacent voxels with different materials.  All faces should be added.
	{
		VoxelGroup group;
		group.voxels.push_back(Voxel(Vec3<int>(0, 0, 0), 0));
		group.voxels.push_back(Voxel(Vec3<int>(0, 0, 1), 1));

		Reference<Indigo::Mesh> data = makeIndigoMeshForVoxelGroup(group, /*subsample_factor=*/1, /*generate_shading_normals=*/false);

		testAssert(data->num_materials_referenced == 2);
		testAssert(data->triangles.size() == 2 * 6 * 2);

		// Test with subsampling
		data = makeIndigoMeshForVoxelGroup(group, /*subsample_factor=*/4, /*generate_shading_normals=*/false);
		testAssert(data->num_materials_referenced <= 2);
		testAssert(data->triangles.size() == 6 * 2);
	}

	// Performance test
	if(true)
	{
		for(int i=0; i<1000; ++i)
		{
			std::vector<uint8> filecontents;
			FileUtils::readEntireFile("N:\\new_cyberspace\\trunk\\testfiles\\voxels\\ob_151064_voxeldata.voxdata", filecontents);

			VoxelGroup group;
			group.voxels.resize(filecontents.size() / sizeof(Voxel));
			testAssert(filecontents.size() == group.voxels.dataSizeBytes());
			std::memcpy(group.voxels.data(), filecontents.data(), filecontents.size());


			conPrint("AABB: " + group.getAABB().toString());
			conPrint("AABB volume: " + toString(group.getAABB().volume()));

			{
				Timer timer;

				Reference<Indigo::Mesh> data = makeIndigoMeshForVoxelGroup(group, /*subsample_factor=*/1, /*generate_shading_normals=*/false);

				conPrint("Meshing of " + toString(group.voxels.size()) + " voxels with subsample_factor=1 took " + timer.elapsedString());
				conPrint("Resulting num tris: " + toString(data->triangles.size()));
			}
			{
				Timer timer;

				Reference<Indigo::Mesh> data = makeIndigoMeshForVoxelGroup(group, /*subsample_factor=*/2, /*generate_shading_normals=*/false);

				conPrint("Meshing of " + toString(group.voxels.size()) + " voxels with subsample_factor=2 took " + timer.elapsedString());
				conPrint("Resulting num tris: " + toString(data->triangles.size()));
			}
		}

		if(false)
		{
			VoxelGroup group;
			for(int z=0; z<100; z += 2)
				for(int y=0; y<100; ++y)
					for(int x=0; x<10; ++x)
						group.voxels.push_back(Voxel(Vec3<int>(x, y, z), 0));


			Timer timer;

			Reference<Indigo::Mesh> data = makeIndigoMeshForVoxelGroup(group, /*subsample_factor=*/1, /*generate_shading_normals=*/false);

			conPrint("Meshing of " + toString(group.voxels.size()) + " voxels took " + timer.elapsedString());
			conPrint("Resulting num tris: " + toString(data->triangles.size()));
		}
	}

	conPrint("VoxelMeshBuilding::test() done.");
}


#endif // BUILD_TESTS
