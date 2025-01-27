/*=====================================================================
PhysicsWorld.cpp
----------------
Copyright Glare Technologies Limited 2022 -
=====================================================================*/
#include "PhysicsWorld.h"


#include <dll/include/IndigoMesh.h>
#include "../graphics/BatchedMesh.h"
#include <simpleraytracer/ray.h>
#include <simpleraytracer/raymesh.h>
#include <utils/StringUtils.h>
#include <utils/ConPrint.h>
#include <utils/Timer.h>
#include <utils/HashMapInsertOnly2.h>
#include <utils/string_view.h>
#include <utils/RuntimeCheck.h>
#include <stdarg.h>
#include <Lock.h>
#include "JoltUtils.h"


#if USE_JOLT
#ifndef NDEBUG
#define JPH_PROFILE_ENABLED 1
#endif
#include <Jolt/Jolt.h>
#include <Jolt/RegisterTypes.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Core/StreamWrapper.h>
#include <Jolt/Physics/PhysicsSettings.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/PhysicsScene.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/ConvexHullShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/Shape/ScaledShape.h>
#include <Jolt/Physics/Collision/Shape/HeightFieldShape.h>
#include <Jolt/Physics/Collision/PhysicsMaterialSimple.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyActivationListener.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseQuery.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/Shape/OffsetCenterOfMassShape.h>
#endif
#include <HashSet.h>
#include <fstream>


#if USE_JOLT
typedef uint32 uint;


// Callback for traces from Jolt, connect this to your own trace function if you have one
static void traceImpl(const char* inFMT, ...)
{
	// Format the message
	va_list list;
	va_start(list, inFMT);
	char buffer[1024];
	vsnprintf(buffer, sizeof(buffer), inFMT, list);

	conPrint(buffer);
}


// Each broadphase layer results in a separate bounding volume tree in the broad phase. You at least want to have
// a layer for non-moving and moving objects to avoid having to update a tree full of static objects every frame.
// You can have a 1-on-1 mapping between object layers and broadphase layers (like in this case) but if you have
// many object layers you'll be creating many broad phase trees, which is not efficient. If you want to fine tune
// your broadphase layers define JPH_TRACK_BROADPHASE_STATS and look at the stats reported on the TTY.
namespace BroadPhaseLayers
{
	static constexpr JPH::BroadPhaseLayer NON_MOVING(0);
	static constexpr JPH::BroadPhaseLayer MOVING(1);
	static constexpr uint32 NUM_LAYERS(2);
};


// BroadPhaseLayerInterface implementation
// This defines a mapping between object and broadphase layers.
class BPLayerInterfaceImpl final : public JPH::BroadPhaseLayerInterface
{
public:
	BPLayerInterfaceImpl()
	{
		// Create a mapping table from object to broad phase layer
		mObjectToBroadPhase[Layers::NON_MOVING] = BroadPhaseLayers::NON_MOVING;
		mObjectToBroadPhase[Layers::MOVING] = BroadPhaseLayers::MOVING;
		mObjectToBroadPhase[Layers::NON_COLLIDABLE] = BroadPhaseLayers::MOVING; // NOTE: this a good thing to do?
	}

	virtual uint32 GetNumBroadPhaseLayers() const override
	{
		return BroadPhaseLayers::NUM_LAYERS;
	}

	virtual JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer inLayer) const override
	{
		assert(inLayer < Layers::NUM_LAYERS);
		return mObjectToBroadPhase[inLayer];
	}

#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
	virtual const char* GetBroadPhaseLayerName(JPH::BroadPhaseLayer inLayer) const override
	{
		switch((JPH::BroadPhaseLayer::Type)inLayer)
		{
		case (JPH::BroadPhaseLayer::Type)BroadPhaseLayers::NON_MOVING:	return "NON_MOVING";
		case (JPH::BroadPhaseLayer::Type)BroadPhaseLayers::MOVING:		return "MOVING";
		default: assert(false);											return "INVALID";
		}
	}
#endif // JPH_EXTERNAL_PROFILE || JPH_PROFILE_ENABLED

private:
	JPH::BroadPhaseLayer					mObjectToBroadPhase[Layers::NUM_LAYERS];
};


class MyBroadPhaseLayerFilter : public JPH::ObjectVsBroadPhaseLayerFilter
{
	/// Returns true if an object layer should collide with a broadphase layer
	virtual bool ShouldCollide(JPH::ObjectLayer inLayer1, JPH::BroadPhaseLayer inLayer2) const
	{
		switch(inLayer1)
		{
		case Layers::NON_MOVING:
			return inLayer2 == BroadPhaseLayers::MOVING;
		case Layers::MOVING:
			return true;
		case Layers::NON_COLLIDABLE:
			return false;
		default:
			assert(false);
			return false;
		}
	}
};


class MyObjectLayerPairFilter : public JPH::ObjectLayerPairFilter
{
	/// Returns true if two layers can collide
	virtual bool ShouldCollide(JPH::ObjectLayer inLayer1, JPH::ObjectLayer inLayer2) const
	{
		switch(inLayer1)
		{
		case Layers::NON_MOVING:
			return inLayer2 == Layers::MOVING; // Non moving only collides with moving
		case Layers::MOVING:
			return inLayer2 != Layers::NON_COLLIDABLE; // Moving collides with everything apart from Layers::NON_COLLIDABLE
		case Layers::NON_COLLIDABLE:
			return false;
		default:
			assert(false);
			return false;
		}
	}
};


#endif // USE_JOLT


void PhysicsWorld::init()
{
#if USE_JOLT
	// Register allocation hook
	JPH::RegisterDefaultAllocator();

	// Install callbacks
	JPH::Trace = traceImpl;
	//JPH_IF_ENABLE_ASSERTS(AssertFailed = AssertFailedImpl;)

	// Create a factory
	JPH::Factory::sInstance = new JPH::Factory();

	// Register all Jolt physics types
	JPH::RegisterTypes();
#endif
}


void PhysicsWorld::setWaterBuoyancyEnabled(bool enabled)
{
	water_buoyancy_enabled = enabled;
}


void PhysicsWorld::setWaterZ(float water_z_)
{
	water_z = water_z_;
}


// Modified from TempAllocatorImpl, added maxTop high water mark.
class PhysicsWorldAllocatorImpl final : public JPH::TempAllocator
{
public:
	JPH_OVERRIDE_NEW_DELETE

	/// Constructs the allocator with a maximum allocatable size of inSize
	explicit PhysicsWorldAllocatorImpl(uint inSize) :
		mBase(static_cast<uint8 *>(JPH::AlignedAllocate(inSize, JPH_RVECTOR_ALIGNMENT))),
		mSize(inSize),
		maxTop(0)
	{
	}

	/// Destructor, frees the block
	virtual	~PhysicsWorldAllocatorImpl() override
	{
		assert(mTop == 0);
		JPH::AlignedFree(mBase);
	}

	// See: TempAllocator
	virtual void* Allocate(uint inSize) override
	{
		if(inSize == 0)
		{
			return nullptr;
		}
		else
		{
			uint new_top = mTop + JPH::AlignUp(inSize, JPH_RVECTOR_ALIGNMENT);
			if(new_top > mSize)
				throw glare::Exception("PhysicsWorldAllocatorImpl: out of memory");//JPH_CRASH; // Out of memory
			void *address = mBase + mTop;
			mTop = new_top;
			maxTop = myMax(maxTop, mTop);
			return address;
		}
	}

	// See: TempAllocator
	virtual void Free(void *inAddress, uint inSize) override
	{
		if(inAddress == nullptr)
		{
			assert(inSize == 0);
		}
		else
		{
			mTop -= JPH::AlignUp(inSize, JPH_RVECTOR_ALIGNMENT);
			if(mBase + mTop != inAddress)
				throw glare::Exception("PhysicsWorldAllocatorImpl: Freeing in the wrong order"); // JPH_CRASH; // Freeing in the wrong order
		}
	}

	// Check if no allocations have been made
	bool IsEmpty() const
	{
		return mTop == 0;
	}

	uint getMaxAllocated() { return maxTop; }

private:
	uint8 *							mBase;							///< Base address of the memory block
	uint							mSize;							///< Size of the memory block
	uint							mTop = 0;						///< Current top of the stack
	uint							maxTop;							// high water mark
};


PhysicsWorld::PhysicsWorld(/*PhysicsWorldBodyActivationCallbacks* activation_callbacks_*/)
:	activated_obs(/*empty_key_=*/NULL),
	newly_activated_obs(/*empty_key_=*/NULL),
	water_buoyancy_enabled(false),
	water_z(0),
	event_listener(NULL)
#if !USE_JOLT
	,ob_grid(/*cell_w=*/32.0, /*num_buckets=*/4096, /*expected_num_items_per_bucket=*/4, /*empty key=*/NULL),
	large_objects(/*empty key=*/NULL, /*expected num items=*/32),
#endif
{
#if USE_JOLT
	// Highest high water mark I have seen so far is 20.5 MB.  
	// Note that increasing mMaxNumHits in CharacterVirtualSettings results in a lot more mem usage.
	temp_allocator = new PhysicsWorldAllocatorImpl(32 * 1024 * 1024);

	// We need a job system that will execute physics jobs on multiple threads. Typically
	// you would implement the JobSystem interface yourself and let Jolt Physics run on top
	// of your own job scheduler. JobSystemThreadPool is an example implementation.
	job_system = new JPH::JobSystemThreadPool(JPH::cMaxPhysicsJobs, JPH::cMaxPhysicsBarriers, JPH::thread::hardware_concurrency() - 1);

	// This is the max amount of rigid bodies that you can add to the physics system. If you try to add more you'll get an error.
	// Note: This value is low because this is a simple test. For a real project use something in the order of 65536.
	const uint32 cMaxBodies = 65536;

	// This determines how many mutexes to allocate to protect rigid bodies from concurrent access. Set it to 0 for the default settings.
	const uint32 cNumBodyMutexes = 0;

	// This is the max amount of body pairs that can be queued at any time (the broad phase will detect overlapping
	// body pairs based on their bounding boxes and will insert them into a queue for the narrowphase). If you make this buffer
	// too small the queue will fill up and the broad phase jobs will start to do narrow phase work. This is slightly less efficient.
	// Note: This value is low because this is a simple test. For a real project use something in the order of 65536.
	const uint32 cMaxBodyPairs = 65536;

	// This is the maximum size of the contact constraint buffer. If more contacts (collisions between bodies) are detected than this
	// number then these contacts will be ignored and bodies will start interpenetrating / fall through the world.
	// Note: This value is low because this is a simple test. For a real project use something in the order of 10240.
	const uint32 cMaxContactConstraints = 10240;

	// Create mapping table from object layer to broadphase layer
	// Note: As this is an interface, PhysicsSystem will take a reference to this so this instance needs to stay alive!
	broad_phase_layer_interface = new BPLayerInterfaceImpl();

	broad_phase_layer_filter = new MyBroadPhaseLayerFilter();

	object_layer_pair_filter = new MyObjectLayerPairFilter();

	// Now we can create the actual physics system.
	physics_system = new JPH::PhysicsSystem();
	physics_system->Init(cMaxBodies, cNumBodyMutexes, cMaxBodyPairs, cMaxContactConstraints, *broad_phase_layer_interface, *broad_phase_layer_filter, *object_layer_pair_filter);

	physics_system->SetGravity(JPH::Vec3Arg(0, 0, -9.81f));

	// A body activation listener gets notified when bodies activate and go to sleep
	// Note that this is called from a job so whatever you do here needs to be thread safe.
	// Registering one is entirely optional.
	physics_system->SetBodyActivationListener(this);

	// A contact listener gets notified when bodies (are about to) collide, and when they separate again.
	// Note that this is called from a job so whatever you do here needs to be thread safe.
	// Registering one is entirely optional.
	physics_system->SetContactListener(this);
#endif
}


PhysicsWorld::~PhysicsWorld()
{
	delete physics_system;
	delete object_layer_pair_filter;
	delete broad_phase_layer_filter;
	delete broad_phase_layer_interface;
	delete job_system;
	delete temp_allocator;
}


void PhysicsWorld::setNewObToWorldTransform(PhysicsObject& object, const Vec4f& translation, const Quatf& rot_quat, const Vec4f& scale)
{
	assert(translation.isFinite());

	object.pos = translation;
	object.rot = rot_quat;
	object.scale = Vec3f(scale);

	if(!object.jolt_body_id.IsInvalid()) // If we are updating Jolt state, and this object has a corresponding Jolt object:
	{
		JPH::BodyInterface& body_interface = physics_system->GetBodyInterface();

		body_interface.SetPositionRotationAndVelocity(object.jolt_body_id, /*pos=*/toJoltVec3(translation),
			/*rot=*/toJoltQuat(rot_quat), /*vel=*/JPH::Vec3(0, 0, 0), /*ang vel=*/JPH::Vec3(0, 0, 0));


		// Update scale if needed.  This is a little complicated because we need to use the ScaledShape decorated shape.
		JPH::RefConst<JPH::Shape> cur_shape = body_interface.GetShape(object.jolt_body_id);
		if(cur_shape->GetSubType() == JPH::EShapeSubType::Scaled) // If current Jolt shape is a scaled shape:
		{
			assert(dynamic_cast<const JPH::ScaledShape*>(cur_shape.GetPtr()));
			const JPH::ScaledShape* cur_scaled_shape = static_cast<const JPH::ScaledShape*>(cur_shape.GetPtr());

			if(toJoltVec3(scale) != cur_scaled_shape->GetScale()) // If scale has changed:
			{
				const JPH::Shape* inner_shape = cur_scaled_shape->GetInnerShape(); // Get inner shape

				JPH::Vec3 use_scale = toJoltVec3(scale);
				if(inner_shape->GetSubType() == JPH::EShapeSubType::Sphere) // HACK: Jolt sphere shapes don't support non-uniform scale, so just force to a uniform scale.
					use_scale = JPH::Vec3(scale[0], scale[0], scale[0]);

				JPH::RefConst<JPH::Shape> new_shape = new JPH::ScaledShape(inner_shape, use_scale); // Make new decorated scaled shape with new scale

				// conPrint("Made new scaled shape for new scale");
				// NOTE: Setting inUpdateMassProperties to false to avoid a crash/assert in Jolt, I think we need to set mass properties somewhere first.
				body_interface.SetShape(object.jolt_body_id, new_shape, /*inUpdateMassProperties=*/false, JPH::EActivation::DontActivate);
			}
		}
		else // Else if current Jolt shape is not a scaled shape:
		{
			// We use OffsetCenterOfMass for vehicles, which have the scale 'built-in' / ignored.  So we don't want to scale the OffsetCenterOfMass shape.
			if(maskWToZero(scale) != Vec4f(1,1,1,0) && cur_shape->GetSubType() != JPH::EShapeSubType::OffsetCenterOfMass) // And scale is != 1 (and shape is not OffsetCenterOfMass that we use for vehicles):
			{
				JPH::Vec3 use_scale = toJoltVec3(scale);
				if(cur_shape->GetSubType() == JPH::EShapeSubType::Sphere) // HACK: Jolt sphere shapes don't support non-uniform scale, so just force to a uniform scale.
					use_scale = JPH::Vec3(scale[0], scale[0], scale[0]);

				JPH::RefConst<JPH::Shape> new_shape = new JPH::ScaledShape(cur_shape, use_scale);

				// conPrint("Changing to scaled shape");
				// NOTE: Setting inUpdateMassProperties to false to avoid a crash/assert in Jolt, I think we need to set mass properties somewhere first.
				body_interface.SetShape(object.jolt_body_id, new_shape, /*inUpdateMassProperties=*/false, JPH::EActivation::DontActivate);
			}
		}

		body_interface.ActivateBody(object.jolt_body_id);
	}
}


void PhysicsWorld::setNewObToWorldTransform(PhysicsObject& object, const Vec4f& pos, const Quatf& rot, const Vec4f& linear_vel, const Vec4f& angular_vel)
{
	assert(pos.isFinite());

	object.pos = pos;
	object.rot = rot;

	if(!object.jolt_body_id.IsInvalid()) // If we are updating Jolt state, and this object has a corresponding Jolt object:
	{
		JPH::BodyInterface& body_interface = physics_system->GetBodyInterface();

		body_interface.SetPositionRotationAndVelocity(object.jolt_body_id, /*pos=*/toJoltVec3(pos),
			/*rot=*/toJoltQuat(rot), /*vel=*/toJoltVec3(linear_vel), /*ang vel=*/toJoltVec3(angular_vel));
	}
}


Vec4f PhysicsWorld::getObjectLinearVelocity(const PhysicsObject& object) const
{
	if(!object.jolt_body_id.IsInvalid()) // If we are updating Jolt state, and this object has a corresponding Jolt object:
	{
		JPH::BodyInterface& body_interface = physics_system->GetBodyInterface();

		return toVec4fVec(body_interface.GetLinearVelocity(object.jolt_body_id));
	}
	else
		return Vec4f(0);
}


void PhysicsWorld::setLinearAndAngularVelToZero(PhysicsObject& object)
{
	if(!object.jolt_body_id.IsInvalid()) // If we are updating Jolt state, and this object has a corresponding Jolt object:
	{
		JPH::BodyInterface& body_interface = physics_system->GetBodyInterface();

		body_interface.SetLinearAndAngularVelocity(object.jolt_body_id, /*vel=*/toJoltVec3(Vec4f(0.f)), /*ang vel=*/toJoltVec3(Vec4f(0.f)));
	}
}


void computeToWorldAndToObMatrices(const Vec4f& translation, const Quatf& rot_quat, const Vec4f& scale, Matrix4f& ob_to_world_out, Matrix4f& world_to_ob_out)
{
	// Don't use a zero scale component, because it makes the matrix uninvertible, which breaks various things, including picking and normals.
	Vec4f use_scale = scale;
	if(use_scale[0] == 0) use_scale[0] = 1.0e-6f;
	if(use_scale[1] == 0) use_scale[1] = 1.0e-6f;
	if(use_scale[2] == 0) use_scale[2] = 1.0e-6f;


	const Matrix4f rot = rot_quat.toMatrix();
	Matrix4f ob_to_world;
	ob_to_world.setColumn(0, rot.getColumn(0) * use_scale[0]);
	ob_to_world.setColumn(1, rot.getColumn(1) * use_scale[1]);
	ob_to_world.setColumn(2, rot.getColumn(2) * use_scale[2]);
	ob_to_world.setColumn(3, setWToOne(translation));

	/*
	inverse:
	= (TRS)^-1
	= S^-1 R^-1 T^-1
	= S^-1 R^T T^-1
	*/
	const Matrix4f rot_inv = rot.getTranspose();
	Matrix4f S_inv_R_inv;

	const Vec4f recip_scale = maskWToZero(div(Vec4f(1.f), use_scale));

	// left-multiplying with a scale matrix is equivalent to multiplying column 0 with the scale vector (s_x, s_y, s_z, 0) etc.
	S_inv_R_inv.setColumn(0, rot_inv.getColumn(0) * recip_scale);
	S_inv_R_inv.setColumn(1, rot_inv.getColumn(1) * recip_scale);
	S_inv_R_inv.setColumn(2, rot_inv.getColumn(2) * recip_scale);
	S_inv_R_inv.setColumn(3, Vec4f(0, 0, 0, 1));

	assert(epsEqual(S_inv_R_inv, Matrix4f::scaleMatrix(recip_scale[0], recip_scale[1], recip_scale[2]) * rot_inv));

	const Matrix4f world_to_ob = rightTranslate(S_inv_R_inv, -translation);

#ifndef NDEBUG
	// Matrix4f prod = ob_to_world * world_to_ob;
	// assert(epsEqual(Matrix4f::identity(), prod, /*eps=*/1.0e-3f));
#endif
	
	ob_to_world_out = ob_to_world;
	world_to_ob_out = world_to_ob;
}


void PhysicsWorld::moveKinematicObject(PhysicsObject& object, const Vec4f& translation, const Quatf& rot, float dt)
{
	if(!object.jolt_body_id.IsInvalid()) // If we are updating Jolt state, and this object has a corresponding Jolt object:
	{
		JPH::BodyInterface& body_interface = physics_system->GetBodyInterface();

		if(body_interface.GetMotionType(object.jolt_body_id) == JPH::EMotionType::Kinematic)
		{
			body_interface.MoveKinematic(object.jolt_body_id, toJoltVec3(translation), toJoltQuat(rot), dt);
		}
		else
		{
			//assert(0); // Tried to move a non-kinematic object with MoveKinematic().  Catch this ourself otherwise jolt crashes.
		}
	}
}


// Just store the original material index, so we can recover it in traceRay().
class SubstrataPhysicsMaterial : public JPH::PhysicsMaterial
{
public:
	//JPH_DECLARE_SERIALIZABLE_VIRTUAL(SubstrataPhysicsMaterial)   // NOTE: need this?

	SubstrataPhysicsMaterial(uint32 index_) : index(index_) {}

	virtual const char *					GetDebugName() const override		{ return "SubstrataPhysicsMaterial"; }

	// See: PhysicsMaterial::SaveBinaryState
	//virtual void							SaveBinaryState(StreamOut &inStream) const override;

protected:
	// See: PhysicsMaterial::RestoreBinaryState
	//virtual void							RestoreBinaryState(StreamIn &inStream) override;

public:
	uint32 index;
};


static size_t computeSizeBForShape(JPH::Ref<JPH::Shape> jolt_shape)
{
	JPH::Shape::VisitedShapes visited_shapes; // Jolt uses this to make sure it doesn't double-count sub-shapes.

	JPH::Shape::Stats shape_stats = jolt_shape->GetStatsRecursive(visited_shapes);

	return shape_stats.mSizeBytes;
}


PhysicsShape PhysicsWorld::createJoltShapeForIndigoMesh(const Indigo::Mesh& mesh, bool build_dynamic_physics_ob)
{
	const Indigo::Vector<Indigo::Vec3f>& verts = mesh.vert_positions;
	const Indigo::Vector<Indigo::Triangle>& tris = mesh.triangles;
	const Indigo::Vector<Indigo::Quad>& quads = mesh.quads;
	
	const size_t verts_size = verts.size();
	const size_t final_num_tris_size = tris.size() + quads.size() * 2;
	
	if(build_dynamic_physics_ob)
	{
		// Jolt doesn't support dynamic triangles mesh shapes, so we need to convert it to a convex hull shape.
		JPH::Array<JPH::Vec3> points(verts_size);

		for(size_t i = 0; i < verts_size; ++i)
		{
			const Indigo::Vec3f& vert = verts[i];
			points[i] = JPH::Vec3(vert.x, vert.y, vert.z);
		}

		JPH::Ref<JPH::ConvexHullShapeSettings> hull_shape_settings = new JPH::ConvexHullShapeSettings(
			points
		);

		JPH::Result<JPH::Ref<JPH::Shape>> result = hull_shape_settings->Create();
		if(result.HasError())
			throw glare::Exception(std::string("Error building Jolt shape: ") + result.GetError().c_str());
		JPH::Ref<JPH::Shape> jolt_shape = result.Get();
		PhysicsShape shape;
		shape.jolt_shape = jolt_shape;
		shape.size_B = computeSizeBForShape(jolt_shape);
		return shape;
	}
	else
	{
		JPH::VertexList vertex_list(verts_size);
		JPH::IndexedTriangleList tri_list(final_num_tris_size);
	
		for(size_t i = 0; i < verts_size; ++i)
		{
			const Indigo::Vec3f& vert = verts[i];
			vertex_list[i] = JPH::Float3(vert.x, vert.y, vert.z);
		}
	
	
		for(size_t i = 0; i < tris.size(); ++i)
		{
			const Indigo::Triangle& tri = tris[i];
	
			const uint use_mat_index = tri.tri_mat_index < 32 ? tri.tri_mat_index : 0; // Jolt has a maximum of 32 materials per mesh
			tri_list[i] = JPH::IndexedTriangle(tri.vertex_indices[0], tri.vertex_indices[1], tri.vertex_indices[2], /*inMaterialIndex=*/use_mat_index);
		}

		for(size_t i = 0; i < quads.size(); ++i)
		{
			const Indigo::Quad& quad = quads[i];

			const uint use_mat_index = quad.mat_index < 32 ? quad.mat_index : 0;
			tri_list[tris.size() + i * 2 + 0] = JPH::IndexedTriangle(quad.vertex_indices[0], quad.vertex_indices[1], quad.vertex_indices[2], /*inMaterialIndex=*/use_mat_index);
			tri_list[tris.size() + i * 2 + 1] = JPH::IndexedTriangle(quad.vertex_indices[0], quad.vertex_indices[2], quad.vertex_indices[3], /*inMaterialIndex=*/use_mat_index);
		}
	
		// Create materials
		const uint32 use_num_mats = myMin(32u, mesh.num_materials_referenced); // Jolt has a maximum of 32 materials per mesh
		JPH::PhysicsMaterialList materials(use_num_mats);
		for(uint32 i = 0; i < use_num_mats; ++i)
			materials[i] = new SubstrataPhysicsMaterial(i);
	
		JPH::Ref<JPH::MeshShapeSettings> mesh_body_settings = new JPH::MeshShapeSettings(vertex_list, tri_list, materials);
		JPH::Result<JPH::Ref<JPH::Shape>> result = mesh_body_settings->Create();
		if(result.HasError())
			throw glare::Exception(std::string("Error building Jolt shape: ") + result.GetError().c_str());
		JPH::Ref<JPH::Shape> jolt_shape = result.Get();
		PhysicsShape shape;
		shape.jolt_shape = jolt_shape;
		shape.size_B = computeSizeBForShape(jolt_shape);
		return shape;
	}
}


inline static Vec4f transformSkinnedVertex(const Vec4f vert_pos, size_t joint_offset_B, size_t weights_offset_B, BatchedMesh::ComponentType joints_component_type, BatchedMesh::ComponentType weights_component_type,
	const js::Vector<Matrix4f, 16>& joint_matrices, const uint8* src_vertex_data, const size_t vert_size_B, size_t i)
{
	// Read joint indices
	uint32 use_joints[4];
	if(joints_component_type == BatchedMesh::ComponentType_UInt8)
	{
		uint8 joints[4];
		std::memcpy(joints, &src_vertex_data[i * vert_size_B + joint_offset_B], sizeof(uint8) * 4);
		for(int z=0; z<4; ++z)
			use_joints[z] = joints[z];
	}
	else
	{
		assert(joints_component_type == BatchedMesh::ComponentType_UInt16);

		uint16 joints[4];
		std::memcpy(joints, &src_vertex_data[i * vert_size_B + joint_offset_B], sizeof(uint16) * 4);
		for(int z=0; z<4; ++z)
			use_joints[z] = joints[z];
	}

	// Read weights
	float use_weights[4];
	if(weights_component_type == BatchedMesh::ComponentType_UInt8)
	{
		uint8 weights[4];
		std::memcpy(weights, &src_vertex_data[i * vert_size_B + weights_offset_B], sizeof(uint8) * 4);
		for(int z=0; z<4; ++z)
			use_weights[z] = weights[z] * (1.0f / 255.f);
	}
	else if(weights_component_type == BatchedMesh::ComponentType_UInt16)
	{
		uint16 weights[4];
		std::memcpy(weights, &src_vertex_data[i * vert_size_B + weights_offset_B], sizeof(uint16) * 4);
		for(int z=0; z<4; ++z)
			use_weights[z] = weights[z] * (1.0f / 65535.f);
	}
	else
	{
		assert(weights_component_type == BatchedMesh::ComponentType_Float);

		std::memcpy(use_weights, &src_vertex_data[i * vert_size_B + weights_offset_B], sizeof(float) * 4);
	}

	for(int z=0; z<4; ++z)
		assert(use_joints[z] < (uint32)joint_matrices.size());
	
	return
		joint_matrices[use_joints[0]] * vert_pos * use_weights[0] + // joint indices should have been bound checked in BatchedMesh::checkValidAndSanitiseMesh()
		joint_matrices[use_joints[1]] * vert_pos * use_weights[1] + 
		joint_matrices[use_joints[2]] * vert_pos * use_weights[2] + 
		joint_matrices[use_joints[3]] * vert_pos * use_weights[3];
}



PhysicsShape PhysicsWorld::createJoltShapeForBatchedMesh(const BatchedMesh& mesh, bool build_dynamic_physics_ob)
{
	const size_t vert_size_B = mesh.vertexSize();
	const size_t num_verts = mesh.numVerts();
	const size_t num_tris = mesh.numIndices() / 3;

	const BatchedMesh::VertAttribute* pos_attr = mesh.findAttribute(BatchedMesh::VertAttribute_Position);
	if(!pos_attr)
		throw glare::Exception("Pos attribute not present.");
	if(pos_attr->component_type != BatchedMesh::ComponentType_Float)
		throw glare::Exception("Pos attribute must have float type.");
	const size_t pos_offset = pos_attr->offset_B;


	// If mesh has joints and weights, take the skinning transform into account.
	const AnimationData& anim_data = mesh.animation_data;

	const bool use_skin_transforms = mesh.findAttribute(BatchedMesh::VertAttribute_Joints) && mesh.findAttribute(BatchedMesh::VertAttribute_Weights) &&
		!anim_data.joint_nodes.empty();

	js::Vector<Matrix4f, 16> joint_matrices;

	size_t joint_offset_B, weights_offset_B;
	BatchedMesh::ComponentType joints_component_type, weights_component_type;
	joint_offset_B = weights_offset_B = 0;
	joints_component_type = weights_component_type = BatchedMesh::ComponentType_UInt8;
	if(use_skin_transforms)
	{
		js::Vector<Matrix4f, 16> node_matrices;

		const size_t num_nodes = anim_data.sorted_nodes.size();
		node_matrices.resizeNoCopy(num_nodes);

		for(size_t n=0; n<anim_data.sorted_nodes.size(); ++n)
		{
			const int node_i = anim_data.sorted_nodes[n];
			runtimeCheck(node_i >= 0 && node_i < (int)anim_data.nodes.size()); // All these indices should have been bound checked in BatchedMesh::readFromData(), check again anyway.
			const AnimationNodeData& node_data = anim_data.nodes[node_i];
			const Vec4f trans = node_data.trans;
			const Quatf rot = node_data.rot;
			const Vec4f scale = node_data.scale;

			const Matrix4f rot_mat = rot.toMatrix();
			const Matrix4f TRS(
				rot_mat.getColumn(0) * copyToAll<0>(scale),
				rot_mat.getColumn(1) * copyToAll<1>(scale),
				rot_mat.getColumn(2) * copyToAll<2>(scale),
				setWToOne(trans));

			runtimeCheck(node_data.parent_index >= -1 && node_data.parent_index < (int)node_matrices.size());
			const Matrix4f node_transform = (node_data.parent_index == -1) ? TRS : (node_matrices[node_data.parent_index] * TRS);
			node_matrices[node_i] = node_transform;
		}

		joint_matrices.resizeNoCopy(anim_data.joint_nodes.size());

		for(size_t i=0; i<anim_data.joint_nodes.size(); ++i)
		{
			const int node_i = anim_data.joint_nodes[i];
			runtimeCheck(node_i >= 0 && node_i < (int)node_matrices.size() && node_i >= 0 && node_i < (int)anim_data.nodes.size());
			joint_matrices[i] = node_matrices[node_i] * anim_data.nodes[node_i].inverse_bind_matrix;
		}

		const BatchedMesh::VertAttribute& joints_attr = mesh.getAttribute(BatchedMesh::VertAttribute_Joints);
		joint_offset_B = joints_attr.offset_B;
		joints_component_type = joints_attr.component_type;
		runtimeCheck(joints_component_type == BatchedMesh::ComponentType_UInt8 || joints_component_type == BatchedMesh::ComponentType_UInt16); // See BatchedMesh::checkValidAndSanitiseMesh().
		runtimeCheck((num_verts - 1) * vert_size_B + joint_offset_B + BatchedMesh::vertAttributeSize(joints_attr) <= mesh.vertex_data.size());

		const BatchedMesh::VertAttribute& weights_attr = mesh.getAttribute(BatchedMesh::VertAttribute_Weights);
		weights_offset_B = weights_attr.offset_B;
		weights_component_type = weights_attr.component_type;
		runtimeCheck(weights_component_type == BatchedMesh::ComponentType_UInt8 || weights_component_type == BatchedMesh::ComponentType_UInt16 || weights_component_type == BatchedMesh::ComponentType_Float); // See BatchedMesh::checkValidAndSanitiseMesh().
		runtimeCheck((num_verts - 1) * vert_size_B + weights_offset_B + BatchedMesh::vertAttributeSize(weights_attr) <= mesh.vertex_data.size());
	}



	if(build_dynamic_physics_ob)
	{
		// Jolt doesn't support dynamic triangles mesh shapes, so we need to convert it to a convex hull shape.
		JPH::Array<JPH::Vec3> points(num_verts);

		const uint8* src_vertex_data = mesh.vertex_data.data();
		for(size_t i = 0; i < num_verts; ++i)
		{
			Vec4f vert_pos(1.f);
			std::memcpy(&vert_pos, src_vertex_data + pos_offset + i * vert_size_B, sizeof(::Vec3f));

			if(use_skin_transforms)
				vert_pos = transformSkinnedVertex(vert_pos, joint_offset_B, weights_offset_B, joints_component_type, weights_component_type, joint_matrices, src_vertex_data, vert_size_B, i);

			points[i] = JPH::Vec3(vert_pos[0], vert_pos[1], vert_pos[2]);
		}

		JPH::Ref<JPH::ConvexHullShapeSettings> hull_shape_settings = new JPH::ConvexHullShapeSettings(
			points
		);

		JPH::Result<JPH::Ref<JPH::Shape>> result = hull_shape_settings->Create();
		if(result.HasError())
			throw glare::Exception(std::string("Error building Jolt shape: ") + result.GetError().c_str());
		JPH::Ref<JPH::Shape> jolt_shape = result.Get();
		PhysicsShape shape;
		shape.jolt_shape = jolt_shape;
		shape.size_B = computeSizeBForShape(jolt_shape);
		return shape;
	}
	else
	{
		JPH::VertexList vertex_list(num_verts);
		JPH::IndexedTriangleList tri_list(num_tris);

		// Copy Vertices
		const uint8* src_vertex_data = mesh.vertex_data.data();
		for(size_t i = 0; i < num_verts; ++i)
		{
			Vec4f vert_pos(1.f);
			std::memcpy(&vert_pos, src_vertex_data + pos_offset + i * vert_size_B, sizeof(::Vec3f));

			if(use_skin_transforms)
				vert_pos = transformSkinnedVertex(vert_pos, joint_offset_B, weights_offset_B, joints_component_type, weights_component_type, joint_matrices, src_vertex_data, vert_size_B, i);

			vertex_list[i] = JPH::Float3(vert_pos[0], vert_pos[1], vert_pos[2]);
		}

		// Copy Triangles
		const BatchedMesh::ComponentType index_type = mesh.index_type;

		const uint8*  const index_data_uint8  = (const uint8* )mesh.index_data.data();
		const uint16* const index_data_uint16 = (const uint16*)mesh.index_data.data();
		const uint32* const index_data_uint32 = (const uint32*)mesh.index_data.data();

		unsigned int dest_tri_i = 0;
		for(size_t b = 0; b < mesh.batches.size(); ++b)
		{
			const size_t tri_begin = mesh.batches[b].indices_start / 3;
			const size_t tri_end   = tri_begin + mesh.batches[b].num_indices / 3;
			const uint32 mat_index = mesh.batches[b].material_index;

			for(size_t t = tri_begin; t < tri_end; ++t)
			{
				uint32 vertex_indices[3];
				if(index_type == BatchedMesh::ComponentType_UInt8)
				{
					vertex_indices[0] = index_data_uint8[t*3 + 0];
					vertex_indices[1] = index_data_uint8[t*3 + 1];
					vertex_indices[2] = index_data_uint8[t*3 + 2];
				}
				else if(index_type == BatchedMesh::ComponentType_UInt16)
				{
					vertex_indices[0] = index_data_uint16[t*3 + 0];
					vertex_indices[1] = index_data_uint16[t*3 + 1];
					vertex_indices[2] = index_data_uint16[t*3 + 2];
				}
				else if(index_type == BatchedMesh::ComponentType_UInt32)
				{
					vertex_indices[0] = index_data_uint32[t*3 + 0];
					vertex_indices[1] = index_data_uint32[t*3 + 1];
					vertex_indices[2] = index_data_uint32[t*3 + 2];
				}
				else
				{
					throw glare::Exception("Invalid index type.");
				}


				const uint use_mat_index = mat_index < 32 ? mat_index : 0;
				tri_list[dest_tri_i] = JPH::IndexedTriangle(vertex_indices[0], vertex_indices[1], vertex_indices[2], /*inMaterialIndex=*/use_mat_index);

				dest_tri_i++;
			}
		}

		// Create materials
		const uint32 use_num_mats = myMin(32u, (uint32)mesh.numMaterialsReferenced());
		JPH::PhysicsMaterialList materials(use_num_mats);
		for(uint32 i = 0; i < use_num_mats; ++i)
			materials[i] = new SubstrataPhysicsMaterial(i);

		JPH::Ref<JPH::MeshShapeSettings> mesh_body_settings = new JPH::MeshShapeSettings(vertex_list, tri_list, materials);
		JPH::Result<JPH::Ref<JPH::Shape>> result = mesh_body_settings->Create();
		if(result.HasError())
			throw glare::Exception(std::string("Error building Jolt shape: ") + result.GetError().c_str());
		JPH::Ref<JPH::Shape> jolt_shape = result.Get();
		PhysicsShape shape;
		shape.jolt_shape = jolt_shape;
		shape.size_B = computeSizeBForShape(jolt_shape);
		return shape;
	}
}


PhysicsShape PhysicsWorld::createJoltHeightFieldShape(int vert_res, const Array2D<float>& heightfield, float quad_w)
{
	const int block_size = 4;

	assert((int)heightfield.getWidth() >= vert_res);
	assert(heightfield.getWidth() % 2 == 0); // Needs to be a multiple of mBlockSize
	assert(heightfield.getWidth() >= 4); // inSampleCount / mBlockSize = inSampleCount / 2 needs to be at least 4, so need inSampleCount >= 4.
	assert(Maths::isPowerOfTwo<int>((int)heightfield.getWidth() / block_size)); // inSampleCount / mBlockSize must be a power of 2 and minimally 2.
	assert((heightfield.getWidth() / block_size) >= 2);

	const float z_offset = -quad_w * (heightfield.getWidth() - 1);
	JPH::HeightFieldShapeSettings settings(
		heightfield.getData(), 
		JPH::Vec3Arg(0,0,z_offset), // inOffset
		JPH::Vec3Arg(quad_w, 1.f, quad_w), // inScale
		(uint32)heightfield.getWidth(), // inSampleCount: inSampleCount / mBlockSize must be a power of 2 and minimally 2.
		NULL // inMaterialIndices
	);
	settings.mBlockSize = block_size;
	//settings.mBitsPerSample = sBitsPerSample;

	JPH::Result<JPH::Ref<JPH::Shape>> result = settings.Create();
	if(result.HasError())
		throw glare::Exception(std::string("Error building Jolt heightfield shape: ") + result.GetError().c_str());

	JPH::Ref<JPH::Shape> jolt_shape = result.Get();

	// const JPH::AABox bounds = jolt_shape->GetLocalBounds();

	PhysicsShape shape;
	shape.jolt_shape = jolt_shape;
	shape.size_B = computeSizeBForShape(jolt_shape);
	return shape;
}


// Creates a box, centered at (0,0,0), with x and y extent = ground_quad_w, and z extent = 1.
PhysicsShape PhysicsWorld::createGroundQuadShape(float ground_quad_w)
{
	JPH::Ref<JPH::BoxShapeSettings> cube_shape_settings = new JPH::BoxShapeSettings(/*inHalfExtent=*/JPH::Vec3(ground_quad_w/2, ground_quad_w/2, 0.5f));

	JPH::Result<JPH::Ref<JPH::Shape>> result = cube_shape_settings->Create();
	if(result.HasError())
		throw glare::Exception(std::string("Error building Jolt shape: ") + result.GetError().c_str());
	JPH::Ref<JPH::Shape> jolt_shape = result.Get();
	PhysicsShape shape;
	shape.jolt_shape = jolt_shape;
	shape.size_B = computeSizeBForShape(jolt_shape);
	return shape;
}


PhysicsShape PhysicsWorld::createCOMOffsetShapeForShape(const PhysicsShape& original_shape, const Vec4f& COM_offset)
{
	JPH::Result<JPH::Ref<JPH::Shape>> result = JPH::OffsetCenterOfMassShapeSettings(
		toJoltVec3(COM_offset),
		original_shape.jolt_shape
	).Create();

	if(result.HasError())
		throw glare::Exception(std::string("Error building Jolt shape: ") + result.GetError().c_str());

	PhysicsShape offset_shape;
	offset_shape.jolt_shape = result.Get();
	offset_shape.size_B = original_shape.size_B;
	return offset_shape;
}


void PhysicsWorld::addObject(const Reference<PhysicsObject>& object)
{
	assert(object->pos.isFinite());
	assert(object->scale.isFinite());
	assert(object->rot.v.isFinite());

	this->objects_set.insert(object);

	if(!object->jolt_body_id.IsInvalid())
		return; // Jolt body is already built, we don't need to do anything more.

	if(fabs(object->pos[0]) > 1.0e9 || fabs(object->pos[1]) > 1.0e9 || fabs(object->pos[2]) > 1.0e9)
	{
		return;
	}

	if(object->scale.x == 0 || object->scale.y == 0 || object->scale.z == 0)
	{
		//assert(0);
		return;
	}

	JPH::BodyInterface& body_interface = physics_system->GetBodyInterface();

	if(object->is_sphere)
	{
		JPH::Ref<JPH::SphereShapeSettings> sphere_shape = new JPH::SphereShapeSettings(0.5f);

		JPH::Ref<JPH::ShapeSettings> final_shape_settings;
		if(object->scale == Vec3f(1.f))
			final_shape_settings = sphere_shape;
		else
			final_shape_settings = new JPH::ScaledShapeSettings(sphere_shape, JPH::Vec3(object->scale[0], object->scale[0], object->scale[0])); // Use uniform scale, sphere shapes must have uniform scale in jolt. 

		JPH::BodyCreationSettings sphere_settings(final_shape_settings,
			JPH::Vec3(object->pos[0], object->pos[1], object->pos[2]),
			JPH::Quat(object->rot.v[0], object->rot.v[1], object->rot.v[2], object->rot.v[3]),
			object->dynamic ? JPH::EMotionType::Dynamic : (object->kinematic ? JPH::EMotionType::Kinematic : JPH::EMotionType::Static), 
			object->dynamic ? Layers::MOVING : (object->collidable ? Layers::NON_MOVING : Layers::NON_COLLIDABLE));

		sphere_settings.mFriction = myClamp(object->friction, 0.f, 1.f);
		sphere_settings.mRestitution = myClamp(object->restitution, 0.f, 1.f);
		sphere_settings.mMassPropertiesOverride.mMass = myMax(0.001f, object->mass);
		sphere_settings.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;

		sphere_settings.mUserData = (uint64)object.ptr();
		
		object->jolt_body_id = body_interface.CreateAndAddBody(sphere_settings, JPH::EActivation::DontActivate);

		//conPrint("Added Jolt sphere body, dynamic: " + boolToString(object->dynamic));
	}
	else if(object->is_cube)
	{
		JPH::Ref<JPH::BoxShapeSettings> cube_shape_settings = new JPH::BoxShapeSettings(JPH::Vec3(0.5f, 0.5f, 0.5f));

		JPH::Ref<JPH::ShapeSettings> final_shape_settings;
		if(object->scale == Vec3f(1.f))
			final_shape_settings = cube_shape_settings;
		else
			final_shape_settings = new JPH::ScaledShapeSettings(cube_shape_settings, JPH::Vec3(object->scale[0], object->scale[1], object->scale[2]));

		// Create the settings for the body itself. Note that here you can also set other properties like the restitution / friction.
		JPH::BodyCreationSettings cube_settings(final_shape_settings,
			JPH::Vec3(object->pos[0], object->pos[1], object->pos[2]),
			JPH::Quat(object->rot.v[0], object->rot.v[1], object->rot.v[2], object->rot.v[3]),
			object->dynamic ? JPH::EMotionType::Dynamic : (object->kinematic ? JPH::EMotionType::Kinematic : JPH::EMotionType::Static), 
			object->dynamic ? Layers::MOVING : (object->collidable ? Layers::NON_MOVING : Layers::NON_COLLIDABLE));

		cube_settings.mFriction = myClamp(object->friction, 0.f, 1.f);
		cube_settings.mRestitution = myClamp(object->restitution, 0.f, 1.f);
		cube_settings.mMassPropertiesOverride.mMass = myMax(0.001f, object->mass);
		cube_settings.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
		cube_settings.mUserData = (uint64)object.ptr();

		object->jolt_body_id = body_interface.CreateAndAddBody(cube_settings, JPH::EActivation::DontActivate);

		//conPrint("Added Jolt cube body, dynamic: " + boolToString(object->dynamic));
	}
	else
	{
		JPH::Ref<JPH::Shape> shape = object->shape.jolt_shape;
		if(shape.GetPtr() == NULL)
			return;

		const bool is_mesh_shape = shape->GetType() == JPH::EShapeType::Mesh;
		assert(!(object->dynamic && is_mesh_shape)); // We should have built a convex hull shape for dynamic objects.

		JPH::Ref<JPH::Shape> final_shape;
		if(object->scale == Vec3f(1.f))
			final_shape = shape;
		else
			final_shape = new JPH::ScaledShape(shape, JPH::Vec3(object->scale[0], object->scale[1], object->scale[2]));

		const JPH::EMotionType motion_type  = (object->dynamic && !is_mesh_shape) ? JPH::EMotionType::Dynamic : (object->kinematic ? JPH::EMotionType::Kinematic : JPH::EMotionType::Static);
		const JPH::ObjectLayer object_layer = (object->dynamic && !is_mesh_shape) ? Layers::MOVING : (object->collidable ? Layers::NON_MOVING : Layers::NON_COLLIDABLE);

		JPH::BodyCreationSettings settings(final_shape,
			JPH::Vec3(object->pos[0], object->pos[1], object->pos[2]),
			JPH::Quat(object->rot.v[0], object->rot.v[1], object->rot.v[2], object->rot.v[3]),
			motion_type, object_layer);
		
		settings.mFriction = myClamp(object->friction, 0.f, 1.f);
		settings.mRestitution = myClamp(object->restitution, 0.f, 1.f);
		settings.mMassPropertiesOverride.mMass = myMax(0.001f, object->mass);
		settings.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;

		settings.mUserData = (uint64)object.ptr();

		object->jolt_body_id = body_interface.CreateAndAddBody(settings, JPH::EActivation::DontActivate);

		//conPrint("Added Jolt mesh body");
	}
}



void PhysicsWorld::removeObject(const Reference<PhysicsObject>& object)
{
	// conPrint("PhysicsWorld::removeObject: " + toHexString((uint64)object.ptr())); 
	JPH::BodyInterface& body_interface = physics_system->GetBodyInterface();

	// Remove jolt body if it exists
	if(!object->jolt_body_id.IsInvalid())
	{
		body_interface.RemoveBody(object->jolt_body_id);

		body_interface.DestroyBody(object->jolt_body_id);

		object->jolt_body_id = JPH::BodyID();

		//conPrint("Removed Jolt body");
	}

	{
		Lock lock(activated_obs_mutex);
		activated_obs.erase(object.ptr()); // Object should have been removed from there when its Jolt body is removed (and deactivated), but do it again to be safe.

		newly_activated_obs.erase(object.ptr());
		//deactivated_obs.erase(object.ptr());
	}

	this->objects_set.erase(object);
}


void PhysicsWorld::think(double dt)
{
	// If you take larger steps than 1 / 60th of a second you need to do multiple collision steps in order to keep the simulation stable. Do 1 collision step per 1 / 60th of a second (round up).
	const int cCollisionSteps = 1;

	// If you want more accurate step results you can do multiple sub steps within a collision step. Usually you would set this to 1.
	const int cIntegrationSubSteps = 1;

	// We simulate the physics world in discrete time steps. 60 Hz is a good rate to update the physics system.
	physics_system->Update((float)dt, cCollisionSteps, cIntegrationSubSteps, temp_allocator, job_system);


	// Apply buoyancy to all activated dynamic objects if enabled
	if(water_buoyancy_enabled)
	{
		//Timer timer;
		Lock lock(activated_obs_mutex);

		const JPH::BodyLockInterfaceLocking& lock_interface = physics_system->GetBodyLockInterface();

		for(auto it = activated_obs.begin(); it != activated_obs.end(); ++it)
		{
			PhysicsObject* physics_ob = *it;

			JPH::Body* body = lock_interface.TryGetBody(physics_ob->jolt_body_id);
			if((body->GetMotionType() == JPH::EMotionType::Dynamic)) // Don't want to apply to our kinematic scripted objects.
			{
				if(body->GetWorldSpaceBounds().mMin.GetZ() < this->water_z) // If bottom of object is < water_z.  (use as quick test for in water)
				{
					const float fluid_density = 1020.f; // water density, kg/m^3, see https://en.wikipedia.org/wiki/Seawater

					// From ApplyBuoyancyImpulse: 
					// fluid_density = buoyancy / (total_volume * inverse_mass)
					// buoyancy = fluid_density * total_volume * inverse_mass
					// buoyancy = fluid_density * total_volume / mass
					const float buoyancy = fluid_density * body->GetShape()->GetVolume() / physics_ob->mass;

					float total_volume, submerged_volume;
					const bool impulse_applied = body->ApplyBuoyancyImpulse(
						JPH::RVec3Arg(0,0, this->water_z), // inSurfacePosition
						JPH::RVec3Arg(0,0,1), // inSurfaceNormal
						buoyancy, // inBuoyancy
						physics_ob->use_zero_linear_drag ? 0.f : 0.1f, // inLinearDrag
						0.2f, // inAngularDrag
						JPH::Vec3Arg(0,0,0), // inFluidVelocity
						JPH::Vec3Arg(0,0,-9.81f), // inGravity
						(float)dt, // inDeltaTime
						total_volume,
						submerged_volume
					);

					// conPrint("submerged_volume: " + ::doubleToStringNSigFigs(submerged_volume, 4) + ", total_volume: " + ::doubleToStringNSigFigs(total_volume, 4));

					if(impulse_applied)
					{
						if(!physics_ob->underwater)
						{
							if(event_listener)
								event_listener->physicsObjectEnteredWater(*physics_ob);
							physics_ob->underwater = true;
						}

						physics_ob->last_submerged_volume = submerged_volume;
					}
					else
					{
						physics_ob->underwater = false;
						physics_ob->last_submerged_volume = 0;
					}
				}
				else 
				{
					if(physics_ob->underwater)
					{
						physics_ob->underwater = false;
						physics_ob->last_submerged_volume = 0;
					}
				}
			}
		}
		//conPrint("Applying buoyancy took " + timer.elapsedStringMSWIthNSigFigs(5));
	}
}


// NOTE: may be called from a Jolt thread!
// "Called whenever a body activates, note this can be called from any thread so make sure your code is thread safe."
void PhysicsWorld::OnBodyActivated(const JPH::BodyID& inBodyID, uint64 inBodyUserData)
{
	//conPrint("Jolt body activated");

	if(inBodyUserData != 0)
	{
		PhysicsObject* physics_ob = (PhysicsObject*)inBodyUserData;

		{
			Lock lock(activated_obs_mutex);
			activated_obs.insert(physics_ob);

			//activation_events.push_back(ActivationEvent({/*type=*/ActivationEvent::ActivationEventType_Activated, physics_ob}));
			newly_activated_obs.insert(physics_ob);
		}
	}
	//activated_obs.insert(inBodyID);
}


// NOTE: may be called from a Jolt thread!
void PhysicsWorld::OnBodyDeactivated(const JPH::BodyID& inBodyID, uint64 inBodyUserData)
{
	//conPrint("Jolt body deactivated");

	if(inBodyUserData != 0)
	{
		PhysicsObject* physics_ob = (PhysicsObject*)inBodyUserData;

		{
			Lock lock(activated_obs_mutex);
			activated_obs.erase(physics_ob);

			//activation_events.push_back(ActivationEvent({/*type=*/ActivationEvent::ActivationEventType_Deactivated, physics_ob }));
			//deactivated_obs.insert(physics_ob);
		}
	}
	//activated_obs.erase(inBodyID);
}


/// Called whenever a new contact point is detected.
/// Note that this callback is called when all bodies are locked, so don't use any locking functions!
/// Body 1 and 2 will be sorted such that body 1 ID < body 2 ID, so body 1 may not be dynamic.
/// Note that only active bodies will report contacts, as soon as a body goes to sleep the contacts between that body and all other
/// bodies will receive an OnContactRemoved callback, if this is the case then Body::IsActive() will return false during the callback.
/// When contacts are added, the constraint solver has not run yet, so the collision impulse is unknown at that point.
/// The velocities of inBody1 and inBody2 are the velocities before the contact has been resolved, so you can use this to
/// estimate the collision impulse to e.g. determine the volume of the impact sound to play (see: EstimateCollisionResponse).

// Note that this is called from a job so whatever you do here needs to be thread safe.
void PhysicsWorld::OnContactAdded(const JPH::Body &inBody1, const JPH::Body &inBody2, const JPH::ContactManifold &inManifold, JPH::ContactSettings &ioSettings)
{
	if(event_listener)
		event_listener->contactAdded(inBody1, inBody2, inManifold);
}


/// Called whenever a contact is detected that was also detected last update.
/// Note that this callback is called when all bodies are locked, so don't use any locking functions!
/// Body 1 and 2 will be sorted such that body 1 ID < body 2 ID, so body 1 may not be dynamic.
/// If the structure of the shape of a body changes between simulation steps (e.g. by adding/removing a child shape of a compound shape),
/// it is possible that the same sub shape ID used to identify the removed child shape is now reused for a different child shape. The physics
/// system cannot detect this, so may send a 'contact persisted' callback even though the contact is now on a different child shape. You can
/// detect this by keeping the old shape (before adding/removing a part) around until the next PhysicsSystem::Update (when the OnContactPersisted
/// callbacks are triggered) and resolving the sub shape ID against both the old and new shape to see if they still refer to the same child shape.

// Note that this is called from a job so whatever you do here needs to be thread safe.
void PhysicsWorld::OnContactPersisted(const JPH::Body &inBody1, const JPH::Body &inBody2, const JPH::ContactManifold &inManifold, JPH::ContactSettings &ioSettings)
{
	if(event_listener)
		event_listener->contactPersisted(inBody1, inBody2, inManifold);
}


void PhysicsWorld::clear()
{
	// TODO: remove all jolt objects

	this->objects_set.clear();
}


PhysicsWorld::MemUsageStats PhysicsWorld::getMemUsageStats() const
{
	HashSet<const JPH::Shape*> meshes(/*empty_key=*/NULL, /*expected_num_items=*/objects_set.size());
	MemUsageStats stats;
	stats.num_meshes = 0;
	stats.mem = 0;

	JPH::Shape::VisitedShapes visited_shapes; // Jolt uses this to make sure it doesn't double-count sub-shapes.
	JPH::BodyInterface& body_interface = physics_system->GetBodyInterface();

	for(auto it = objects_set.begin(); it != objects_set.end(); ++it)
	{
		const PhysicsObject* ob = it->getPointer();

		const JPH::Shape* shape = body_interface.GetShape(ob->jolt_body_id).GetPtr(); // Get actual possibly-decorated shape used.
		if(shape)
		{
			const bool added = meshes.insert(shape).second;
			if(added)
			{
				JPH::Shape::Stats shape_stats = shape->GetStatsRecursive(visited_shapes);

				stats.mem += shape_stats.mSizeBytes;
			}
		}
	}

	for(auto it = visited_shapes.begin(); it != visited_shapes.end(); ++it)
	{
		const JPH::Shape* shape = *it;
		if(dynamic_cast<const JPH::MeshShape*>(shape))
			stats.num_meshes++;
	}

	return stats;
}


std::string PhysicsWorld::getDiagnostics() const
{
	const MemUsageStats stats = getMemUsageStats();
	std::string s;
	s += "Objects: " + toString(objects_set.size()) + "\n";
	s += "Jolt bodies: " + toString(this->physics_system->GetNumBodies()) + "\n";
	{
		Lock lock(activated_obs_mutex);
		s += "Active bodies: " + toString(this->activated_obs.size()) + "\n";
	}
	s += "Meshes:  " + toString(stats.num_meshes) + "\n";
	s += "mem usage: " + getNiceByteSize(stats.mem) + "\n";

	assert(dynamic_cast<PhysicsWorldAllocatorImpl*>(temp_allocator));
	s += "temp allocator max usage: " + getNiceByteSize(static_cast<PhysicsWorldAllocatorImpl*>(temp_allocator)->getMaxAllocated()) + "\n";

	return s;
}


std::string PhysicsWorld::getLoadedMeshes() const
{
	std::string s;
	HashMapInsertOnly2<const RayMesh*, int64> meshes(/*empty key=*/NULL, objects_set.size());
	for(auto it = objects_set.begin(); it != objects_set.end(); ++it)
	{
		//const PhysicsObject* ob = it->getPointer();
		//const bool added = meshes.insert(std::make_pair(ob->shape->raymesh.ptr(), 0)).second;
		//if(added)
		//{
		//	s += ob->shape->raymesh->getName() + "\n";
		//}
	}

	return s;
}


const Vec4f PhysicsWorld::getPosInJolt(const Reference<PhysicsObject>& object)
{
	JPH::BodyInterface& body_interface = physics_system->GetBodyInterface();

	const JPH::Vec3 pos = body_interface.GetPosition(object->jolt_body_id);

	return toVec4fPos(pos);
}


void PhysicsWorld::traceRay(const Vec4f& origin, const Vec4f& dir, float max_t, RayTraceResult& results_out) const
{
	results_out.hit_object = NULL;

	const JPH::RRayCast ray(toJoltVec3(origin), toJoltVec3(dir * max_t));
	JPH::RayCastResult hit_result;
	const bool found_hit = this->physics_system->GetNarrowPhaseQuery().CastRay(ray, hit_result);
	if(found_hit)
	{
		// Lock the body.  Use locking interface so we can call body->GetWorldSpaceSurfaceNormal().
		JPH::BodyLockRead lock(physics_system->GetBodyLockInterfaceNoLock(), hit_result.mBodyID);
		assert(lock.Succeeded()); // When this runs all bodies are locked so this should not fail

		const JPH::Body* body = &lock.GetBody();

		const uint64 user_data = body->GetUserData();
		if(user_data != 0)
		{
			results_out.hit_object = (PhysicsObject*)user_data;
			results_out.coords = Vec2f(0.f);
			results_out.hit_t = hit_result.mFraction * max_t;
			results_out.hit_normal_ws = toVec4fVec(body->GetWorldSpaceSurfaceNormal(hit_result.mSubShapeID2, ray.GetPointOnRay(hit_result.mFraction)));

			const JPH::PhysicsMaterial* mat = body->GetShape()->GetMaterial(hit_result.mSubShapeID2);
			const SubstrataPhysicsMaterial* submat = dynamic_cast<const SubstrataPhysicsMaterial*>(mat);
			results_out.hit_mat_index = submat ? submat->index : 0;

			// conPrint("Hit object, hitdist_ws: " + toString(results_out.hitdist_ws) + ", hit_tri_index: " + toString(results_out.hit_tri_index));
		}
	}
}


bool PhysicsWorld::doesRayHitAnything(const Vec4f& origin, const Vec4f& dir, float max_t) const
{
	const JPH::RRayCast ray(toJoltVec3(origin), toJoltVec3(dir * max_t));
	JPH::RayCastResult hit_result;
	const bool found_hit = this->physics_system->GetNarrowPhaseQuery().CastRay(ray, hit_result);
	return found_hit;
}


void PhysicsWorld::writeJoltSnapshotToDisk(const std::string& path)
{
	// Convert physics system to scene
	JPH::Ref<JPH::PhysicsScene> scene = new JPH::PhysicsScene();
	scene->FromPhysicsSystem(this->physics_system);

	// Save scene
	std::ofstream stream(path.c_str(), std::ofstream::out | std::ofstream::trunc | std::ofstream::binary);
	JPH::StreamOutWrapper wrapper(stream);
	if(stream.is_open())
		scene->SaveBinaryState(wrapper, /*inSaveShapes=*/true, /*inSaveGroupFilter=*/true);
}


#if BUILD_TESTS


#include <simpleraytracer/raymesh.h>
#include <utils/TaskManager.h>
#include <maths/PCG32.h>
#include <utils/TestUtils.h>
#include <utils/StandardPrintOutput.h>
#include <utils/ShouldCancelCallback.h>
#include <graphics/FormatDecoderGLTF.h>


void PhysicsWorld::test()
{
	conPrint("PhysicsWorld::test()");

	// PhysicsWorld::init() needs to have been called already.

	try
	{

		
		//BatchedMesh::readFromFile(TestUtils::getTestReposDir() + "/testfiles/gltf/concept_bike.glb", mesh);
		GLTFLoadedData data;
		//BatchedMeshRef mesh = FormatDecoderGLTF::loadGLBFile(TestUtils::getTestReposDir() + "/testfiles/gltf/concept_bike.glb", data);
		BatchedMeshRef mesh = FormatDecoderGLTF::loadGLBFile(TestUtils::getTestReposDir() + "/testfiles/gltf/2CylinderEngine.glb", data);


		glare::TaskManager task_manager(0);
		StandardPrintOutput print_output;
		DummyShouldCancelCallback should_cancel_callback;

		/*{
			double min_time = 1.0e10;
			for(int i=0; i<1000; ++i)
			{

				RayMesh raymesh("sdfdsf", false);
				raymesh.fromBatchedMesh(mesh);

				Timer timer;
				// Geometry::BuildOptions options;
				// options.compute_is_planar = false;
				// raymesh.build(options, should_cancel_callback, print_output, false, task_manager);

				min_time = myMin(min_time, timer.elapsed());
				conPrint("raymesh.build took " + timer.elapsedStringNPlaces(4) + ", min time so far: " + doubleToStringNDecimalPlaces(min_time, 4) + " s");
			}
		}*/




		double min_time = 1.0e10;
		for(int i=0; i<1; ++i)
		{
			Timer timer;
			auto res = createJoltShapeForBatchedMesh(*mesh, /*is dynamic=*/false);
			min_time = myMin(min_time, timer.elapsed());
			conPrint("createJoltShapeForBatchedMesh took " + timer.elapsedStringNPlaces(4) + ", min time so far: " + doubleToStringNDecimalPlaces(min_time, 4) + " s");
		}
	}
	catch(glare::Exception& e)
	{
		failTest(e.what());
	}

	conPrint("PhysicsWorld::test() done");
}


#endif // BUILD_TESTS
