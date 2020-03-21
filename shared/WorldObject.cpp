/*=====================================================================
WorldObject.cpp
---------------
Copyright Glare Technologies Limited 2016 -
=====================================================================*/
#include "WorldObject.h"


#include <Exception.h>
#include <StringUtils.h>
#include <FileUtils.h>
#include <ConPrint.h>
#include <FileChecksum.h>
#include <Sort.h>
#include <BufferInStream.h>
#if GUI_CLIENT
#include "opengl/OpenGLEngine.h"
#include <SceneNodeModel.h>
#endif
#include "../gui_client/PhysicsObject.h"
#include "../gui_client/WinterShaderEvaluator.h"
#include "../shared/ResourceManager.h"
#include <zstd.h>


WorldObject::WorldObject()
{
	creator_id = UserID::invalidUserID();
	flags = COLLIDABLE_FLAG;

	object_type = ObjectType_Generic;
	from_remote_transform_dirty = false;
	from_remote_other_dirty = false;
	from_local_transform_dirty = false;
	from_local_other_dirty = false;
	using_placeholder_model = false;
#if GUI_CLIENT
	is_selected = false;
#endif
	next_snapshot_i = 0;
	//last_snapshot_time = 0;

	instance_index = 0;
	num_instances = 0;
	translation = Vec4f(0.f);
}


WorldObject::~WorldObject()
{

}


void WorldObject::appendDependencyURLs(std::vector<std::string>& URLs_out)
{
	if(!model_url.empty())
		URLs_out.push_back(model_url);

	for(size_t i=0; i<materials.size(); ++i)
		materials[i]->appendDependencyURLs(URLs_out);
}


void WorldObject::getDependencyURLSet(std::set<std::string>& URLS_out)
{
	std::vector<std::string> URLs;
	this->appendDependencyURLs(URLs);

	URLS_out = std::set<std::string>(URLs.begin(), URLs.end());
}


void WorldObject::convertLocalPathsToURLS(ResourceManager& resource_manager)
{
	if(FileUtils::fileExists(this->model_url)) // If the URL is a local path:
		this->model_url = resource_manager.URLForPathAndHash(this->model_url, FileChecksum::fileChecksum(this->model_url));

	for(size_t i=0; i<materials.size(); ++i)
		materials[i]->convertLocalPathsToURLS(resource_manager);
}


void WorldObject::setTransformAndHistory(const Vec3d& pos_, const Vec3f& axis_, float angle_)
{
	pos = pos_;
	axis = axis_;
	angle = angle_;

	for(int i=0; i<HISTORY_BUF_SIZE; ++i)
	{
		pos_snapshots[i] = pos_;
		axis_snapshots[i] = axis_;
		angle_snapshots[i] = angle_;
		snapshot_times[i] = 0;
	}
}


void WorldObject::setPosAndHistory(const Vec3d& pos_)
{
	pos = pos_;

	for(int i=0; i<HISTORY_BUF_SIZE; ++i)
		pos_snapshots[i] = pos_;
}


void WorldObject::getInterpolatedTransform(double cur_time, Vec3d& pos_out, Vec3f& axis_out, float& angle_out) const
{
	/*
	Timeline: check marks are snapshots received:

	|---------------|----------------|---------------|----------------|
	                                                                       ^
	                                                                      cur_time
	                                                                  ^
	                                               ^                last snapshot
	                                             cur_time - send_period * delay_factor


	When 'frac' gets > 1:
	|---------------|----------------|---------------|----------------x
	                                                                       ^
	                                                                      cur_time
	                                                 ^
	                                     ^         last snapshot
	                                   cur_time - send_period * delay_factor


	 So should be alright as long as it doesn't exceed 2.
	
	|---------------|----------------|---------------|---------------------------|
	                                                                       ^
	                                                                      cur_time
	                                                 ^
	                                     ^         last snapshot
	                                   cur_time - send_period * delay_factor

	
	*/


	const double send_period = 0.1; // Time between update messages from server
	const double delay = send_period * 2.0; // Objects are rendered using the interpolated state at this past time.

	const double delayed_time = cur_time - delay;
	// Search through history for first snapshot
	int begin = 0;
	for(int i=(int)next_snapshot_i-HISTORY_BUF_SIZE; i<(int)next_snapshot_i; ++i)
	{
		const int modi = Maths::intMod(i, HISTORY_BUF_SIZE);
		if(snapshot_times[modi] > delayed_time)
		{
			begin = Maths::intMod(modi - 1, HISTORY_BUF_SIZE);
			break;
		}
	}

	const int end = Maths::intMod(begin + 1, HISTORY_BUF_SIZE);

	// Snapshot times may be the same if we haven't received updates for this object yet.
	const float t  = (snapshot_times[end] == snapshot_times[begin]) ? 0.f : (float)((delayed_time - snapshot_times[begin]) / (snapshot_times[end] - snapshot_times[begin])); // Interpolation fraction

	pos_out   = Maths::uncheckedLerp(pos_snapshots  [begin], pos_snapshots  [end], t);
	axis_out  = Maths::uncheckedLerp(axis_snapshots [begin], axis_snapshots [end], t);
	angle_out = Maths::uncheckedLerp(angle_snapshots[begin], angle_snapshots[end], t);

	if(axis_out.length2() < 1.0e-10f)
	{
		axis_out = Vec3f(0,0,1);
		angle_out = 0;
	}

	//const int last_snapshot_i = next_snapshot_i - 1;

	//const double frac = (cur_time - last_snapshot_time) / send_period; // Fraction of send period ahead of last_snapshot cur time is
	//printVar(frac);
	//if(frac > 2.0f)
	//	int a = 9;
	//const double delayed_state_pos = (double)last_snapshot_i + frac - delay; // Delayed state position in normalised period coordinates.
	//const int delayed_state_begin_snapshot_i = myClamp(Maths::floorToInt(delayed_state_pos), last_snapshot_i - HISTORY_BUF_SIZE + 1, last_snapshot_i);
	//const int delayed_state_end_snapshot_i   = myClamp(delayed_state_begin_snapshot_i + 1,   last_snapshot_i - HISTORY_BUF_SIZE + 1, last_snapshot_i);
	//const float t  = delayed_state_pos - delayed_state_begin_snapshot_i; // Interpolation fraction

	//const int begin = Maths::intMod(delayed_state_begin_snapshot_i, HISTORY_BUF_SIZE);
	//const int end   = Maths::intMod(delayed_state_end_snapshot_i,   HISTORY_BUF_SIZE);

	//pos_out   = Maths::uncheckedLerp(pos_snapshots  [begin], pos_snapshots  [end], t);
	//axis_out  = Maths::uncheckedLerp(axis_snapshots [begin], axis_snapshots [end], t);
	//angle_out = Maths::uncheckedLerp(angle_snapshots[begin], angle_snapshots[end], t);

	//if(axis_out.length2() < 1.0e-10f)
	//{
	//	axis_out = Vec3f(0,0,1);
	//	angle_out = 0;
	//}


	//const double send_period = 0.1; // Time between update messages from server
	//const double delay = /*send_period * */2.0; // Objects are rendered using the interpolated state at this past time.

	//const int last_snapshot_i = next_snapshot_i - 1;

	//const double frac = (cur_time - last_snapshot_time) / send_period; // Fraction of send period ahead of last_snapshot cur time is
	//printVar(frac);
	//if(frac > 2.0f)
	//	int a = 9;
	//const double delayed_state_pos = (double)last_snapshot_i + frac - delay; // Delayed state position in normalised period coordinates.
	//const int delayed_state_begin_snapshot_i = myClamp(Maths::floorToInt(delayed_state_pos), last_snapshot_i - HISTORY_BUF_SIZE + 1, last_snapshot_i);
	//const int delayed_state_end_snapshot_i   = myClamp(delayed_state_begin_snapshot_i + 1,   last_snapshot_i - HISTORY_BUF_SIZE + 1, last_snapshot_i);
	//const float t  = delayed_state_pos - delayed_state_begin_snapshot_i; // Interpolation fraction

	//const int begin = Maths::intMod(delayed_state_begin_snapshot_i, HISTORY_BUF_SIZE);
	//const int end   = Maths::intMod(delayed_state_end_snapshot_i,   HISTORY_BUF_SIZE);

	//pos_out   = Maths::uncheckedLerp(pos_snapshots  [begin], pos_snapshots  [end], t);
	//axis_out  = Maths::uncheckedLerp(axis_snapshots [begin], axis_snapshots [end], t);
	//angle_out = Maths::uncheckedLerp(angle_snapshots[begin], angle_snapshots[end], t);

	//if(axis_out.length2() < 1.0e-10f)
	//{
	//	axis_out = Vec3f(0,0,1);
	//	angle_out = 0;
	//}
}


std::string WorldObject::objectTypeString(ObjectType t)
{
	switch(t)
	{
	case ObjectType_Generic: return "generic";
	case ObjectType_Hypercard: return "hypercard";
	case ObjectType_VoxelGroup: return "voxel group";
	default: return "Unknown";
	}
}


static const uint32 WORLD_OBJECT_SERIALISATION_VERSION = 12;
/*
Version history:
9: introduced voxels
10: changed script_url to script
11: Added flags
12: Added compressed voxel field.
*/


static_assert(sizeof(Voxel) == sizeof(int)*4, "sizeof(Voxel) == sizeof(int)*4");


void writeToStream(const WorldObject& world_ob, OutStream& stream)
{
	// Write version
	stream.writeUInt32(WORLD_OBJECT_SERIALISATION_VERSION);

	writeToStream(world_ob.uid, stream);
	stream.writeUInt32((uint32)world_ob.object_type);
	stream.writeStringLengthFirst(world_ob.model_url);

	// Write materials
	stream.writeUInt32((uint32)world_ob.materials.size());
	for(size_t i=0; i<world_ob.materials.size(); ++i)
		writeToStream(*world_ob.materials[i], stream);

	stream.writeStringLengthFirst(world_ob.script);
	stream.writeStringLengthFirst(world_ob.content);
	stream.writeStringLengthFirst(world_ob.target_url);

	writeToStream(world_ob.pos, stream);
	writeToStream(world_ob.axis, stream);
	stream.writeFloat(world_ob.angle);
	writeToStream(world_ob.scale, stream);

	world_ob.created_time.writeToStream(stream); // new in v5
	writeToStream(world_ob.creator_id, stream); // new in v5

	stream.writeUInt32(world_ob.flags); // new in v11

	if(world_ob.object_type == WorldObject::ObjectType_VoxelGroup)
	{
		// Write compressed voxel data
		stream.writeUInt32((uint32)world_ob.compressed_voxels.size());
		if(world_ob.compressed_voxels.size() > 0)
			stream.writeData(world_ob.compressed_voxels.data(), world_ob.compressed_voxels.dataSizeBytes());
	}
}


void readFromStream(InStream& stream, WorldObject& ob)
{
	// Read version
	const uint32 v = stream.readUInt32();
	if(v > WORLD_OBJECT_SERIALISATION_VERSION)
		throw Indigo::Exception("Unsupported version " + toString(v) + ", expected " + toString(WORLD_OBJECT_SERIALISATION_VERSION) + ".");

	ob.uid = readUIDFromStream(stream);

	if(v >= 7)
		ob.object_type = (WorldObject::ObjectType)stream.readUInt32(); // TODO: handle invalid values?

	ob.model_url = stream.readStringLengthFirst(10000);
	//if(v >= 2)
	//	ob.material_url = stream.readStringLengthFirst(10000);
	if(v >= 2)
	{
		const size_t num_mats = stream.readUInt32();
		ob.materials.resize(num_mats);
		for(size_t i=0; i<ob.materials.size(); ++i)
		{
			if(ob.materials[i].isNull())
				ob.materials[i] = new WorldMaterial();
			readFromStream(stream, *ob.materials[i]);
		}
	}

	if(v >= 4 && v < 10)
	{
		stream.readStringLengthFirst(10000); // read and discard script URL
	}
	else if(v >= 10)
	{
		ob.script = stream.readStringLengthFirst(10000);
	}

	if(v >= 6)
		ob.content = stream.readStringLengthFirst(10000);

	if(v >= 8)
		ob.target_url = stream.readStringLengthFirst(10000);

	ob.pos = readVec3FromStream<double>(stream);
	ob.axis = readVec3FromStream<float>(stream);
	ob.angle = stream.readFloat();

	if(v >= 3)
		ob.scale = readVec3FromStream<float>(stream);
	else
		ob.scale = Vec3f(1.f);

	if(v >= 5)
	{
		ob.created_time.readFromStream(stream);
		ob.creator_id = readUserIDFromStream(stream);
	}
	else
	{
		ob.created_time = TimeStamp::currentTime();
		ob.creator_id = UserID::invalidUserID();
	}

	if(v >= 11)
		ob.flags = stream.readUInt32();

	if(v >= 9 && ob.object_type == WorldObject::ObjectType_VoxelGroup)
	{
		if(v <= 11)
		{
			// Read num voxels
			const uint32 num_voxels = stream.readUInt32();
			if(num_voxels > 1000000)
				throw Indigo::Exception("Invalid num voxels: " + toString(num_voxels));

			ob.voxel_group.voxels.resize(num_voxels);

			// Read voxel data
			if(num_voxels > 0)
				stream.readData(ob.voxel_group.voxels.data(), sizeof(Voxel) * num_voxels);
		}
		else
		{
			// Read compressed voxel data
			const uint32 voxel_data_size = stream.readUInt32();
			if(voxel_data_size > 1000000)
				throw Indigo::Exception("Invalid voxel_data_size: " + toString(voxel_data_size));

			// Read voxel data
			ob.compressed_voxels.resize(voxel_data_size);
			if(voxel_data_size > 0)
				stream.readData(ob.compressed_voxels.data(), voxel_data_size);
		}
	}


	// Set ephemeral state
	ob.state = WorldObject::State_Alive;
}


void writeToNetworkStream(const WorldObject& world_ob, OutStream& stream) // Write without version
{
	writeToStream(world_ob.uid, stream);
	stream.writeUInt32((uint32)world_ob.object_type);
	stream.writeStringLengthFirst(world_ob.model_url);

	// Write materials
	stream.writeUInt32((uint32)world_ob.materials.size());
	for(size_t i=0; i<world_ob.materials.size(); ++i)
		writeToStream(*world_ob.materials[i], stream);

	stream.writeStringLengthFirst(world_ob.script);
	stream.writeStringLengthFirst(world_ob.content);
	stream.writeStringLengthFirst(world_ob.target_url);

	writeToStream(world_ob.pos, stream);
	writeToStream(world_ob.axis, stream);
	stream.writeFloat(world_ob.angle);
	writeToStream(world_ob.scale, stream);

	world_ob.created_time.writeToStream(stream); // new in v5
	writeToStream(world_ob.creator_id, stream); // new in v5

	stream.writeUInt32(world_ob.flags); // new in v11

	stream.writeStringLengthFirst(world_ob.creator_name);

	if(world_ob.object_type == WorldObject::ObjectType_VoxelGroup)
	{
		// Write compressed voxel data
		stream.writeUInt32((uint32)world_ob.compressed_voxels.size());
		if(world_ob.compressed_voxels.size() > 0)
			stream.writeData(world_ob.compressed_voxels.data(), world_ob.compressed_voxels.dataSizeBytes());
	}
}


void readFromNetworkStreamGivenUID(InStream& stream, WorldObject& ob) // UID will have been read already
{
	ob.object_type = (WorldObject::ObjectType)stream.readUInt32(); // TODO: handle invalid values?
	ob.model_url = stream.readStringLengthFirst(10000);
	//if(v >= 2)
	{
		const size_t num_mats = stream.readUInt32();
		ob.materials.resize(num_mats);
		for(size_t i=0; i<ob.materials.size(); ++i)
		{
			if(ob.materials[i].isNull())
				ob.materials[i] = new WorldMaterial();
			readFromStream(stream, *ob.materials[i]);
		}
	}

	ob.script = stream.readStringLengthFirst(10000);
	ob.content = stream.readStringLengthFirst(10000);
	ob.target_url = stream.readStringLengthFirst(10000);

	ob.pos = readVec3FromStream<double>(stream);
	ob.axis = readVec3FromStream<float>(stream);
	ob.angle = stream.readFloat();

	//if(v >= 3)
		ob.scale = readVec3FromStream<float>(stream);

	ob.created_time.readFromStream(stream);
	ob.creator_id = readUserIDFromStream(stream);

	ob.flags = stream.readUInt32();

	ob.creator_name = stream.readStringLengthFirst(10000);

	if(ob.object_type == WorldObject::ObjectType_VoxelGroup)
	{
		// Read compressed voxel data
		const uint32 voxel_data_size = stream.readUInt32();
		if(voxel_data_size > 1000000)
			throw Indigo::Exception("Invalid voxel_data_size (too large): " + toString(voxel_data_size));

		// Read voxel data
		ob.compressed_voxels.resize(voxel_data_size);
		if(voxel_data_size > 0)
			stream.readData(ob.compressed_voxels.data(), voxel_data_size);
	}

	// Set ephemeral state
	//ob.state = WorldObject::State_Alive;
}


const Matrix4f obToWorldMatrix(const WorldObject& ob)
{
	const Vec4f pos((float)ob.pos.x, (float)ob.pos.y, (float)ob.pos.z, 1.f);

	return Matrix4f::translationMatrix(pos + ob.translation) *
		Matrix4f::rotationMatrix(normalise(ob.axis.toVec4fVector()), ob.angle) *
		Matrix4f::scaleMatrix(ob.scale.x, ob.scale.y, ob.scale.z);
}


struct GetMatIndex
{
	size_t operator() (const Voxel& v)
	{
		return (size_t)v.mat_index;
	}
};


void WorldObject::compressVoxelGroup(const VoxelGroup& group, js::Vector<uint8, 16>& compressed_data_out)
{
	size_t max_bucket = 0;
	for(size_t i=0; i<group.voxels.size(); ++i)
		max_bucket = myMax<size_t>(max_bucket, group.voxels[i].mat_index);

	const size_t num_buckets = max_bucket + 1;

	// Step 1: sort by materials
	std::vector<Voxel> sorted_voxels(group.voxels.size());
	Sort::serialCountingSortWithNumBuckets(group.voxels.data(), sorted_voxels.data(), group.voxels.size(), num_buckets, GetMatIndex());

	//std::vector<Voxel> sorted_voxels = group.voxels;
	//std::sort(sorted_voxels.begin(), sorted_voxels.end(), VoxelComparator());


	// Count num items in each bucket
	std::vector<size_t> counts(num_buckets, 0);
	for(size_t i=0; i<group.voxels.size(); ++i)
		counts[group.voxels[i].mat_index]++;

	Vec3<int> current_pos(0, 0, 0);
	int v_i = 0;

	js::Vector<int, 16> data(1 + (int)counts.size() + group.voxels.size() * 3);
	size_t write_i = 0;

	data[write_i++] = (int)counts.size(); // Write num materials

	for(size_t z=0; z<counts.size(); ++z)
	{
		const int count = (int)counts[z];
		data[write_i++] = count; // Wriite count of voxels with that material

		for(size_t i=0; i<count; ++i)
		{
			Vec3<int> relative_pos = sorted_voxels[v_i].pos - current_pos;
			//conPrint("relative_pos: " + relative_pos.toString());

			data[write_i++] = relative_pos.x;
			data[write_i++] = relative_pos.y;
			data[write_i++] = relative_pos.z;

			current_pos = sorted_voxels[v_i].pos;

			v_i++;
		}
	}

	assert(write_i == data.size());

	const size_t compressed_bound = ZSTD_compressBound(data.size() * sizeof(int));

	compressed_data_out.resizeNoCopy(compressed_bound);
	
	const size_t compressed_size = ZSTD_compress(compressed_data_out.data(), compressed_data_out.size(), data.data(), data.dataSizeBytes(),
		ZSTD_CLEVEL_DEFAULT // compression level
	);

	compressed_data_out.resize(compressed_size);

	// conPrint("uncompressed size:      " + toString(group.voxels.size() * sizeof(Voxel)) + " B");
	// conPrint("compressed_size:        " + toString(compressed_size) + " B");
	// const double ratio = (double)group.voxels.size() * sizeof(Voxel) / compressed_size;
	// conPrint("compression ratio: " + toString(ratio));

	//TEMP: decompress and check we get the same value
#ifndef NDEBUG
	VoxelGroup group2;
	decompressVoxelGroup(compressed_data_out.data(), compressed_data_out.size(), group2);
	assert(group2.voxels == sorted_voxels);
#endif
}


void WorldObject::decompressVoxelGroup(const uint8* compressed_data, size_t compressed_data_len, VoxelGroup& group_out)
{
	const uint64 decompressed_size = ZSTD_getDecompressedSize(compressed_data, compressed_data_len);

	//js::Vector<int, 16> decompressed_data(compressed_data_len);
	BufferInStream instream;
	instream.buf.resizeNoCopy(decompressed_size);

	//ZSTD_decompress(decompressed_data.data(), decompressed_size, compressed_data, compressed_data_len);
	ZSTD_decompress(instream.buf.data(), decompressed_size, compressed_data, compressed_data_len);

	//size_t read_i = 0;

	//int num_mats;
	//std::memcpy(&num_mats, &decompressed_data[read_i++], sizeof(int));
	Vec3<int> current_pos(0, 0, 0);

	const int num_mats = instream.readInt32();
	for(int m=0; m<num_mats; ++m)
	{
		const int count = instream.readInt32();
		for(int i=0; i<count; ++i)
		{
			Vec3<int> relative_pos;
			instream.readData(&relative_pos, sizeof(Vec3<int>));

			const Vec3<int> pos = current_pos + relative_pos;

			group_out.voxels.push_back(Voxel(pos, m));

			current_pos = pos;
		}
	}

	if(!instream.endOfStream())
		throw Indigo::Exception("Didn't reach EOF while reading voxels.");
}


void WorldObject::compressVoxels()
{
	if(!this->voxel_group.voxels.empty())
	{
		compressVoxelGroup(this->voxel_group, this->compressed_voxels);
	}
	else
		this->compressed_voxels.clear();
}


void WorldObject::decompressVoxels()
{ 
	if(!this->compressed_voxels.empty())
		decompressVoxelGroup(this->compressed_voxels.data(), this->compressed_voxels.size(), this->voxel_group);
	else
		this->voxel_group.voxels.clear();
}
