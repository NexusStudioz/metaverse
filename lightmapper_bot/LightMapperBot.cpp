/*=====================================================================
LightMapperBot.h
----------------
Copyright Glare Technologies Limited 2020 -
=====================================================================*/


#include "../shared/Protocol.h"
#include "../shared/ResourceManager.h"
#include "../shared/VoxelMeshBuilding.h"
#include "../gui_client/ClientThread.h"
#include "../gui_client/DownloadResourcesThread.h"
#include "../gui_client/NetDownloadResourcesThread.h"
#include "../gui_client/UploadResourceThread.h"
#include <networking/networking.h>
#include <networking/TLSSocket.h>
#include <PlatformUtils.h>
#include <Clock.h>
#include <Timer.h>
#include <ConPrint.h>
#include <OpenSSL.h>
#include <JSONParser.h>
#include <Exception.h>
#include <TaskManager.h>
#include <StandardPrintOutput.h>
#include <FileChecksum.h>
#include <FileUtils.h>
#include <GlareProcess.h>
#include <networking/url.h>
#define USE_INDIGO_SDK 1
// Indigo SDK headers:
#if USE_INDIGO_SDK
#include <dll/include/IndigoMesh.h>
#include <dll/include/IndigoException.h>
#include <dll/include/IndigoMaterial.h>
#include <dll/include/SceneNodeModel.h>
#include <dll/include/SceneNodeRenderSettings.h>
#include <dll/include/SceneNodeRoot.h>
#endif
#include <simpleraytracer/raymesh.h>
#include <graphics/BatchedMesh.h>
#include <indigo/UVUnwrapper.h>


static const std::string username = "lightmapperbot";
static const std::string password = "3NzpaTM37N";


static const std::string toStdString(const Indigo::String& s)
{
	return std::string(s.dataPtr(), s.length());
}

static const Indigo::String toIndigoString(const std::string& s)
{
	return Indigo::String(s.c_str(), s.length());
}


class LightMapperBot
{
public:

	LightMapperBot(const std::string& server_hostname_, int server_port_, ResourceManagerRef& resource_manager_)
	:	server_hostname(server_hostname_), server_port(server_port_), resource_manager(resource_manager_)
	{
		resource_download_thread_manager.addThread(new DownloadResourcesThread(&msg_queue, resource_manager, server_hostname, server_port, &this->num_non_net_resources_downloading));

		for(int i=0; i<4; ++i)
			net_resource_download_thread_manager.addThread(new NetDownloadResourcesThread(&msg_queue, resource_manager, &num_net_resources_downloading));
	}



	void startDownloadingResource(const std::string& url)
	{
		//conPrint("-------------------MainWindow::startDownloadingResource()-------------------\nURL: " + url);

		ResourceRef resource = resource_manager->getResourceForURL(url);
		if(resource->getState() != Resource::State_NotPresent) // If it is getting downloaded, or is downloaded:
		{
			conPrint("Already present or being downloaded, skipping...");
			return;
		}

		try
		{
			const URL parsed_url = URL::parseURL(url);

			if(parsed_url.scheme == "http" || parsed_url.scheme == "https")
			{
				this->net_resource_download_thread_manager.enqueueMessage(new DownloadResourceMessage(url));
				num_net_resources_downloading++;
			}
			else
				this->resource_download_thread_manager.enqueueMessage(new DownloadResourceMessage(url));
		}
		catch(glare::Exception& e)
		{
			conPrint("Failed to parse URL '" + url + "': " + e.what());
		}
	}


	// For every resource that the object uses (model, textures etc..), if the resource is not present locally, start downloading it.
	void startDownloadingResourcesForObject(WorldObject* ob)
	{
		std::set<std::string> dependency_URLs;
		ob->getDependencyURLSet(dependency_URLs);
		for(auto it = dependency_URLs.begin(); it != dependency_URLs.end(); ++it)
		{
			const std::string& url = *it;
			if(!resource_manager->isFileForURLPresent(url))
				startDownloadingResource(url);
		}
	}


	bool allResourcesPresentForOb(WorldObject* ob)
	{
		std::set<std::string> dependency_URLs;
		ob->getDependencyURLSet(dependency_URLs);
		for(auto it = dependency_URLs.begin(); it != dependency_URLs.end(); ++it)
		{
			const std::string& url = *it;
			if(!resource_manager->isFileForURLPresent(url))
				return false;
		}
		return true;
	}


	// From ModelLoading
	void checkValidAndSanitiseMesh(Indigo::Mesh& mesh)
	{
		if(mesh.num_uv_mappings > 10)
			throw glare::Exception("Too many UV sets: " + toString(mesh.num_uv_mappings) + ", max is " + toString(10));

		/*	if(mesh.vert_normals.size() == 0)
		{
		for(size_t i = 0; i < mesh.vert_positions.size(); ++i)
		{
		this->vertices[i].pos.set(mesh.vert_positions[i].x, mesh.vert_positions[i].y, mesh.vert_positions[i].z);
		this->vertices[i].normal.set(0.f, 0.f, 0.f);
		}

		vertex_shading_normals_provided = false;
		}
		else
		{
		assert(mesh.vert_normals.size() == mesh.vert_positions.size());

		for(size_t i = 0; i < mesh.vert_positions.size(); ++i)
		{
		this->vertices[i].pos.set(mesh.vert_positions[i].x, mesh.vert_positions[i].y, mesh.vert_positions[i].z);
		this->vertices[i].normal.set(mesh.vert_normals[i].x, mesh.vert_normals[i].y, mesh.vert_normals[i].z);

		assert(::isFinite(mesh.vert_normals[i].x) && ::isFinite(mesh.vert_normals[i].y) && ::isFinite(mesh.vert_normals[i].z));
		}

		vertex_shading_normals_provided = true;
		}*/


		// Check any supplied normals are valid.
		for(size_t i=0; i<mesh.vert_normals.size(); ++i)
		{
			const float len2 = mesh.vert_normals[i].length2();
			if(!::isFinite(len2))
				mesh.vert_normals[i] = Indigo::Vec3f(1, 0, 0);
			else
			{
				// NOTE: allow non-unit normals?
			}
		}

		// Copy UVs from Indigo::Mesh
		assert(mesh.num_uv_mappings == 0 || (mesh.uv_pairs.size() % mesh.num_uv_mappings == 0));

		// Check all UVs are not NaNs, as NaN UVs cause NaN filtered texture values, which cause a crash in TextureUnit table look-up.  See https://bugs.glaretechnologies.com/issues/271
		const size_t uv_size = mesh.uv_pairs.size();
		for(size_t i=0; i<uv_size; ++i)
		{
			if(!isFinite(mesh.uv_pairs[i].x))
				mesh.uv_pairs[i].x = 0;
			if(!isFinite(mesh.uv_pairs[i].y))
				mesh.uv_pairs[i].y = 0;
		}

		const uint32 num_uv_groups = (mesh.num_uv_mappings == 0) ? 0 : ((uint32)mesh.uv_pairs.size() / mesh.num_uv_mappings);
		const uint32 num_verts = (uint32)mesh.vert_positions.size();

		// Tris
		for(size_t i = 0; i < mesh.triangles.size(); ++i)
		{
			const Indigo::Triangle& src_tri = mesh.triangles[i];

			// Check vertex indices are in bounds
			for(unsigned int v = 0; v < 3; ++v)
				if(src_tri.vertex_indices[v] >= num_verts)
					throw glare::Exception("Triangle vertex index is out of bounds.  (vertex index=" + toString(mesh.triangles[i].vertex_indices[v]) + ", num verts: " + toString(num_verts) + ")");

			// Check uv indices are in bounds
			if(mesh.num_uv_mappings > 0)
				for(unsigned int v = 0; v < 3; ++v)
					if(src_tri.uv_indices[v] >= num_uv_groups)
						throw glare::Exception("Triangle uv index is out of bounds.  (uv index=" + toString(mesh.triangles[i].uv_indices[v]) + ")");
		}

		// Quads
		for(size_t i = 0; i < mesh.quads.size(); ++i)
		{
			// Check vertex indices are in bounds
			for(unsigned int v = 0; v < 4; ++v)
				if(mesh.quads[i].vertex_indices[v] >= num_verts)
					throw glare::Exception("Quad vertex index is out of bounds.  (vertex index=" + toString(mesh.quads[i].vertex_indices[v]) + ")");

			// Check uv indices are in bounds
			if(mesh.num_uv_mappings > 0)
				for(unsigned int v = 0; v < 4; ++v)
					if(mesh.quads[i].uv_indices[v] >= num_uv_groups)
						throw glare::Exception("Quad uv index is out of bounds.  (uv index=" + toString(mesh.quads[i].uv_indices[v]) + ")");
		}
	}

	// Without translation
	static const Matrix4f obToWorldMatrix(const WorldObject* ob)
	{
		return Matrix4f::rotationMatrix(normalise(ob->axis.toVec4fVector()), ob->angle) *
			Matrix4f::scaleMatrix(ob->scale.x, ob->scale.y, ob->scale.z);
	}


	inline static Indigo::Vec3d toIndigoVec3d(const Vec3d& c)
	{
		return Indigo::Vec3d(c.x, c.y, c.z);
	}


	void addObjectToIndigoSG(WorldObject* ob)
	{

	}

	void buildLightMapForOb(WorldState& world_state, WorldObject* ob_to_lightmap)
	{
		try
		{
			conPrint("\n\n\n");
			conPrint("=================== Building lightmap for object ====================");
			conPrint("UID: " + ob_to_lightmap->uid.toString());
			conPrint("model_url: " + ob_to_lightmap->model_url);

			// Hold the world state lock while we process the object and build the indigo scene from it.
			UID ob_uid;
			std::string scene_path;
			{
				Lock lock(world_state.mutex);

				ob_uid = ob_to_lightmap->uid;
				
				// Clear LIGHTMAP_NEEDS_COMPUTING_FLAG.
				// We do this here, so other clients can re-set the LIGHTMAP_NEEDS_COMPUTING_FLAG while we are baking the lightmap, which means that the
				// lightmap will re-bake when done.
				{
					BitUtils::zeroBit(ob_to_lightmap->flags, WorldObject::LIGHTMAP_NEEDS_COMPUTING_FLAG);

					// Enqueue ObjectFlagsChanged
					SocketBufferOutStream packet(SocketBufferOutStream::DontUseNetworkByteOrder);
					packet.writeUInt32(Protocol::ObjectFlagsChanged);
					writeToStream(ob_to_lightmap->uid, packet);
					packet.writeUInt32(ob_to_lightmap->flags);

					this->client_thread->enqueueDataToSend(packet);
				}


				// Iterate over all objects and work out which objects should be in the Indigo scene for the lightmap calc.
				std::set<WorldObjectRef> obs_to_render;
				for(auto it = world_state.objects.begin(); it != world_state.objects.end(); ++it)
				{
					// TEMP: add all objects
					obs_to_render.insert(it->second);
				}

				// Start downloading any resources we don't have that the object uses.
				for(auto it = obs_to_render.begin(); it != obs_to_render.end(); ++it)
				{
					startDownloadingResourcesForObject(it->ptr());
				}

				// Wait until we have downloaded all resources for the object

				Timer wait_timer;
				while(1)
				{
					bool all_present = true;
					for(auto it = obs_to_render.begin(); it != obs_to_render.end(); ++it)
						if(!allResourcesPresentForOb(it->ptr()))
							all_present = false;

					if(all_present)
						break;

					PlatformUtils::Sleep(50);

					if(wait_timer.elapsed() > 30)
						throw glare::Exception("Failed to download all resources for objects");// with UID " + ob->uid.toString());
				}


				// Load mesh from disk:
				// If batched mesh (bmesh), convert to indigo mesh
				// If indigo mesh, can use directly

				Indigo::MeshRef ob_to_lightmap_indigo_mesh;
				{
					
					if(ob_to_lightmap->object_type == WorldObject::ObjectType_VoxelGroup) // If voxel object, convert to mesh
					{
						ob_to_lightmap->decompressVoxels();

						BatchedMeshRef batched_mesh = VoxelMeshBuilding::makeBatchedMeshForVoxelGroup(ob_to_lightmap->getDecompressedVoxelGroup());
						ob_to_lightmap_indigo_mesh = new Indigo::Mesh();
						batched_mesh->buildIndigoMesh(*ob_to_lightmap_indigo_mesh);
					}
					else
					{
						const std::string model_path = resource_manager->pathForURL(ob_to_lightmap->model_url);

						if(hasExtension(model_path, "igmesh"))
						{
							try
							{
								Indigo::Mesh::readFromFile(toIndigoString(model_path), *ob_to_lightmap_indigo_mesh);
							}
							catch(Indigo::IndigoException& e)
							{
								throw glare::Exception(toStdString(e.what()));
							}
						}
						else if(hasExtension(model_path, "bmesh"))
						{
							BatchedMeshRef batched_mesh = new BatchedMesh();
							BatchedMesh::readFromFile(model_path, *batched_mesh);

							ob_to_lightmap_indigo_mesh = new Indigo::Mesh();
							batched_mesh->buildIndigoMesh(*ob_to_lightmap_indigo_mesh);
						}
						else
							throw glare::Exception("unhandled model format: " + model_path);

						checkValidAndSanitiseMesh(*ob_to_lightmap_indigo_mesh); // Throws Indigo::Exception on invalid mesh.
					}

					// See if this object has a lightmap-suitable UV map already
					const bool has_lightmap_uvs = ob_to_lightmap_indigo_mesh->num_uv_mappings >= 2; // TEMP
					if(!has_lightmap_uvs)
					{
						// Generate lightmap UVs
						StandardPrintOutput print_output;
						UVUnwrapper::build(*ob_to_lightmap_indigo_mesh, print_output); // Adds UV set to indigo_mesh.

						if(ob_to_lightmap->object_type != WorldObject::ObjectType_VoxelGroup) // If voxel object, don't update to an unwrapped mesh, rather keep voxels.
						{
							// Convert indigo_mesh to a BatchedMesh.
							// This will also merge verts with the same pos and normal.
							BatchedMeshRef batched_mesh = new BatchedMesh();
							batched_mesh->buildFromIndigoMesh(*ob_to_lightmap_indigo_mesh);

							// Save as bmesh in temp location
							const std::string bmesh_disk_path = PlatformUtils::getTempDirPath() + "/lightmapper_bot_temp.bmesh";

							BatchedMesh::WriteOptions write_options;
							write_options.compression_level = 20;
							batched_mesh->writeToFile(bmesh_disk_path);

							// Compute hash over model
							const uint64 model_hash = FileChecksum::fileChecksum(bmesh_disk_path);

							//const std::string original_filename = FileUtils::getFilename(d.result_path); // Use the original filename, not 'temp.igmesh'.
							const std::string mesh_URL = ResourceManager::URLForNameAndExtensionAndHash("unwrapped"/*original_filename*/, "bmesh", model_hash);

							// Copy model to local resources dir.  UploadResourceThread will read from here.
							this->resource_manager->copyLocalFileToResourceDir(bmesh_disk_path, mesh_URL);

							ob_to_lightmap->model_url = mesh_URL;

							// Send the updated object, with the new model URL, to the server.

							// Enqueue ObjectFullUpdate
							SocketBufferOutStream packet(SocketBufferOutStream::DontUseNetworkByteOrder);
							packet.writeUInt32(Protocol::ObjectFullUpdate);
							ob_to_lightmap->writeToNetworkStream(packet);

							this->client_thread->enqueueDataToSend(packet);

							// Spawn an UploadResourceThread to upload the new model
							resource_upload_thread_manager.addThread(new UploadResourceThread(&this->msg_queue, this->resource_manager->pathForURL(mesh_URL), mesh_URL, server_hostname, server_port, username, password));
						}
					}
				}



				//------------------ Make an Indigo scene graph to light the model, then save it to disk ---------------------
				Indigo::SceneNodeUID light_map_baking_ob_uid;

				Indigo::SceneNodeRootRef root_node = new Indigo::SceneNodeRoot();

				for(auto it = obs_to_render.begin(); it != obs_to_render.end(); ++it)
				{
					WorldObject* ob = it->ptr();

					
					Indigo::MeshRef indigo_mesh;
					if(ob == ob_to_lightmap)
					{
						indigo_mesh = ob_to_lightmap_indigo_mesh;
					}
					else
					{
						if(ob->object_type == WorldObject::ObjectType_VoxelGroup) // If voxel object, convert to mesh
						{
							ob->decompressVoxels();
							BatchedMeshRef batched_mesh = VoxelMeshBuilding::makeBatchedMeshForVoxelGroup(ob->getDecompressedVoxelGroup());
							indigo_mesh = new Indigo::Mesh();
							batched_mesh->buildIndigoMesh(*indigo_mesh);
						}
						else
						{
							const std::string model_path = resource_manager->pathForURL(ob->model_url);

							if(hasExtension(model_path, "igmesh"))
							{
								try
								{
									Indigo::Mesh::readFromFile(toIndigoString(model_path), *indigo_mesh);
								}
								catch(Indigo::IndigoException& e)
								{
									throw glare::Exception(toStdString(e.what()));
								}
							}
							else if(hasExtension(model_path, "bmesh"))
							{
								BatchedMeshRef batched_mesh = new BatchedMesh();
								BatchedMesh::readFromFile(model_path, *batched_mesh);

								indigo_mesh = new Indigo::Mesh();
								batched_mesh->buildIndigoMesh(*indigo_mesh);
							}
							else
								throw glare::Exception("unhandled model format: " + model_path);
						}
					}

					checkValidAndSanitiseMesh(*indigo_mesh); // Throws Indigo::Exception on invalid mesh.

					Indigo::SceneNodeMeshRef mesh_node = new Indigo::SceneNodeMesh(indigo_mesh);

					// Make Indigo materials from loaded parcel mats
					Indigo::Vector<Indigo::SceneNodeMaterialRef> indigo_mat_nodes;
					for(size_t i=0; i<ob->materials.size(); ++i)
					{
						const WorldMaterialRef parcel_mat = ob->materials[i];

						Reference<Indigo::WavelengthDependentParam> albedo_param;
						if(!parcel_mat->colour_texture_url.empty())
						{
							const std::string path = resource_manager->pathForURL(parcel_mat->colour_texture_url);
							albedo_param = new Indigo::TextureWavelengthDependentParam(Indigo::Texture(toIndigoString(path)), new Indigo::RGBSpectrum(Indigo::Vec3d(1.0), /*gamma=*/2.2));
						}
						else
						{
							albedo_param = new Indigo::ConstantWavelengthDependentParam(new Indigo::RGBSpectrum(Indigo::Vec3d(parcel_mat->colour_rgb.r, parcel_mat->colour_rgb.g, parcel_mat->colour_rgb.b), /*gamma=*/2.2));
						}

						Indigo::DiffuseMaterialRef indigo_mat = new Indigo::DiffuseMaterial(
							albedo_param
						);
						indigo_mat->name = toIndigoString(parcel_mat->name);

						indigo_mat_nodes.push_back(new Indigo::SceneNodeMaterial(indigo_mat));
					}

					Indigo::SceneNodeModelRef model_node = new Indigo::SceneNodeModel();
					model_node->setMaterials(indigo_mat_nodes);
					model_node->setGeometry(mesh_node);
					model_node->keyframes = Indigo::Vector<Indigo::KeyFrame>(1, Indigo::KeyFrame(
							0.0,
							toIndigoVec3d(ob->pos),
							Indigo::AxisAngle::identity()
					));
					model_node->rotation = new Indigo::MatrixRotation(obToWorldMatrix(ob).getUpperLeftMatrix().e);
					root_node->addChildNode(model_node);

					if(ob == ob_to_lightmap)
						light_map_baking_ob_uid = model_node->getUniqueID();
				}

				Indigo::SceneNodeRenderSettingsRef settings_node = Indigo::SceneNodeRenderSettings::getDefaults();
				settings_node->untonemapped_scale.setValue(1.0e-9);
				settings_node->width.setValue(512);
				settings_node->height.setValue(512);
				settings_node->bidirectional.setValue(false);
				settings_node->metropolis.setValue(false);
				settings_node->gpu.setValue(true);
				settings_node->light_map_baking_ob_uid.setValue(light_map_baking_ob_uid.value()); // Enable light map baking
				settings_node->generate_lightmap_uvs.setValue(false);
				settings_node->capture_direct_sun_illum.setValue(false);
				settings_node->image_save_period.setValue(2);
				settings_node->save_png.setValue(false);
				settings_node->merging.setValue(false); // Needed for now
				root_node->addChildNode(settings_node);

				Indigo::SceneNodeTonemappingRef tone_mapping = new Indigo::SceneNodeTonemapping();
				tone_mapping->setType(Indigo::SceneNodeTonemapping::Reinhard);
				tone_mapping->pre_scale = 1;
				tone_mapping->post_scale = 1;
				tone_mapping->burn = 6;
				root_node->addChildNode(tone_mapping);

				Indigo::SceneNodeCameraRef cam = new Indigo::SceneNodeCamera();
				cam->lens_radius = 0.0001;
				cam->autofocus = false;
				cam->exposure_duration = 1.0 / 30.0;
				cam->focus_distance = 2.0;
				cam->forwards = Indigo::Vec3d(0, 1, 0);
				cam->up = Indigo::Vec3d(0, 0, 1);
				cam->setPos(Indigo::Vec3d(0, -2, 0.1));
				root_node->addChildNode(cam);

				Reference<Indigo::SunSkyMaterial> sun_sky_mat = new Indigo::SunSkyMaterial();
				const float sun_phi = 1.f; // See MainWindow.cpp
				const float sun_theta = Maths::pi<float>() / 4;
				sun_sky_mat->sundir = normalise(Indigo::Vec3d(std::cos(sun_phi) * std::sin(sun_theta), std::sin(sun_phi) * sun_theta, std::cos(sun_theta)));
				sun_sky_mat->model = "captured-simulation";
				Indigo::SceneNodeBackgroundSettingsRef background_node = new Indigo::SceneNodeBackgroundSettings(sun_sky_mat);
				root_node->addChildNode(background_node);

				root_node->finalise(".");

				scene_path = PlatformUtils::getAppDataDirectory("Cyberspace") + "/lightmap_baking.igs";
		
				// Write Indigo scene to disk.
				root_node->writeToXMLFileOnDisk(
					toIndigoString(scene_path),
					false, // write_absolute_dependency_paths
					NULL // progress_listener
				);

				conPrint("Wrote scene to '" + scene_path + "'.");
			
			} // Release world state lock


			const std::string lightmap_exr_path = PlatformUtils::getAppDataDirectory("Cyberspace") + "/lightmap.exr";
			int lightmap_index = 0;
			if(true)
			{
				const std::string indigo_exe_path = "C:\\programming\\indigo\\output\\vs2019\\indigo_x64\\RelWithDebInfo\\indigo_gui.exe";
				std::vector<std::string> command_line_args;
				command_line_args.push_back(indigo_exe_path);
				command_line_args.push_back(scene_path);
				command_line_args.push_back("--noninteractive");
				command_line_args.push_back("-uexro");
				command_line_args.push_back(lightmap_exr_path);
				command_line_args.push_back("-halt");
				command_line_args.push_back("20");
				glare::Process indigo_process(indigo_exe_path, command_line_args);

				Timer timer;
				while(1)
				{
					while(indigo_process.isStdOutReadable())
					{
						const std::string output = indigo_process.readStdOut();
						std::vector<std::string> lines = ::split(output, '\n');
						for(size_t i=0; i<lines.size(); ++i)
							if(!isAllWhitespace(lines[i]))
								conPrint("INDIGO> " + lines[i]);

						for(size_t i=0; i<lines.size(); ++i)
							if(hasPrefix(lines[i], "Saving untone-mapped EXR to"))
							{
								compressAndUploadLightmap(lightmap_exr_path, ob_uid, lightmap_index);
							}
					}

					// Check to see if the object has been modified, and the lightmap baking needs to be re-started:
					if(lightmap_index >= 1)
					{
						Lock lock(world_state.mutex);
						auto res = world_state.objects.find(ob_uid);
						if(res != world_state.objects.end())
						{
							WorldObjectRef ob2 = res->second;
							if(BitUtils::isBitSet(ob2->flags, WorldObject::LIGHTMAP_NEEDS_COMPUTING_FLAG))
							{
								conPrint("Object has been modified since bake started, aborting bake...");
								indigo_process.terminateProcess();
								return;
							}
						}
					}

					if(!indigo_process.isProcessAlive())
						break;

					PlatformUtils::Sleep(10);
				}

				std::string output, err_output;
				indigo_process.readAllRemainingStdOutAndStdErr(output, err_output);
				conPrint("INDIGO> " + output);
				conPrint("INDIGO> " + err_output);

				conPrint("Indigo process terminated.");
			}
		}
		catch(PlatformUtils::PlatformUtilsExcep& e)
		{
			throw glare::Exception(e.what());
		}
	}


	void compressAndUploadLightmap(const std::string& lightmap_exr_path, UID ob_uid, int& lightmap_index)
	{
		const std::string lightmap_ktx_path = ::removeDotAndExtension(lightmap_exr_path) + "_" + toString(lightmap_index) + ".ktx";
		lightmap_index++;

		//================== Run Compressonator to compress the lightmap EXR with BC6 compression into a KTX file. ========================
		{
			const std::string compressonator_path = PlatformUtils::findProgramOnPath("CompressonatorCLI.exe");
			std::vector<std::string> command_line_args;
			command_line_args.push_back(compressonator_path);
			command_line_args.push_back("-fd"); // Specifies the destination texture format to use
			command_line_args.push_back("BC6H");
			command_line_args.push_back("-mipsize");
			command_line_args.push_back("1");
			command_line_args.push_back(lightmap_exr_path); // input path
			command_line_args.push_back(lightmap_ktx_path); // output path
			glare::Process compressonator_process(compressonator_path, command_line_args);

			Timer timer;
			while(1)
			{
				while(compressonator_process.isStdOutReadable())
				{
					const std::string output = compressonator_process.readStdOut();
					std::vector<std::string> lines = ::split(output, '\n');
					for(size_t i=0; i<lines.size(); ++i)
					{
						//conPrint("COMPRESS> " + lines[i]);
					}
				}

				if(!compressonator_process.isProcessAlive())
					break;

				PlatformUtils::Sleep(1);
			}

			std::string output, err_output;
			compressonator_process.readAllRemainingStdOutAndStdErr(output, err_output);
			//conPrint("COMPRESS> " + output);
			if(!isAllWhitespace(err_output))
				conPrint("COMPRESS error output> " + err_output);

			if(compressonator_process.getExitCode() != 0)
				throw glare::Exception("compressonator execution returned a non-zero code: " + toString(compressonator_process.getExitCode()));

			//conPrint("Compressonator finished.");
		}

		// Compute hash over lightmap
		const uint64 lightmap_hash = FileChecksum::fileChecksum(lightmap_ktx_path);

		const std::string lightmap_URL = ResourceManager::URLForNameAndExtensionAndHash("lightmap", ::getExtension(lightmap_ktx_path), lightmap_hash);

		// Enqueue ObjectLightmapURLChanged
		SocketBufferOutStream packet(SocketBufferOutStream::DontUseNetworkByteOrder);
		packet.writeUInt32(Protocol::ObjectLightmapURLChanged);
		writeToStream(ob_uid, packet);
		packet.writeStringLengthFirst(lightmap_URL);

		this->client_thread->enqueueDataToSend(packet);

		// Spawn an UploadResourceThread to upload the new lightmap
		//conPrint("Uploading lightmap '" + lightmap_ktx_path + "' to the server with URL '" + lightmap_URL + "'...");
		resource_upload_thread_manager.addThread(new UploadResourceThread(&this->msg_queue, lightmap_ktx_path, lightmap_URL, server_hostname, server_port, username, password));
	}


	void doLightMapping(WorldState& world_state, Reference<ClientThread>& client_thread_)
	{
		conPrint("---------------doLightMapping()-----------------");
		this->client_thread = client_thread_;

		try
		{
			//============= Do an initial scan over all objects, to see if any of them need lightmapping ===========
			conPrint("Doing initial scan over all objects...");
			std::set<WorldObjectRef> obs_to_lightmap;
			{
				Lock lock(world_state.mutex);

				for(auto it = world_state.objects.begin(); it != world_state.objects.end(); ++it)
				{
					WorldObject* ob = it->second.ptr();
					conPrint("Checking object with UID " + ob->uid.toString());
					if(/*!ob->model_url.empty() && */BitUtils::isBitSet(ob->flags, WorldObject::LIGHTMAP_NEEDS_COMPUTING_FLAG))
					{
						// Decompress voxel group
						ob->decompressVoxels();

						obs_to_lightmap.insert(ob);
					}
				}
			}

			// Now that we have released the world_state.mutex lock, build lightmaps
			for(auto it = obs_to_lightmap.begin(); it != obs_to_lightmap.end(); ++it)
				buildLightMapForOb(world_state, it->ptr());
			obs_to_lightmap.clear();


			conPrint("Done initial scan over all objects.");

			//============= Now loop and wait for any objects to be marked dirty, and check those objects for if they need lightmapping ===========
			while(1)
			{
				//conPrint("Checking dirty set...");
				{
					Lock lock(world_state.mutex);

					for(auto it = world_state.dirty_from_remote_objects.begin(); it != world_state.dirty_from_remote_objects.end(); ++it)
					{
						WorldObject* ob = it->ptr();
						//conPrint("Found object with UID " + ob->uid.toString() + " in dirty set.");
						//conPrint("LIGHTMAP_NEEDS_COMPUTING_FLAG: " + boolToString(BitUtils::isBitSet(ob->flags, WorldObject::LIGHTMAP_NEEDS_COMPUTING_FLAG)));

						if(/*!ob->model_url.empty() && */BitUtils::isBitSet(ob->flags, WorldObject::LIGHTMAP_NEEDS_COMPUTING_FLAG))
						{
							// Decompress voxel group
							ob->decompressVoxels();

							obs_to_lightmap.insert(ob);
						}
					}

					world_state.dirty_from_remote_objects.clear();
				}

				// Now that we have released the world_state.mutex lock, build lightmaps
				for(auto it = obs_to_lightmap.begin(); it != obs_to_lightmap.end(); ++it)
					buildLightMapForOb(world_state, it->ptr());
				obs_to_lightmap.clear();


				PlatformUtils::Sleep(100);
			}
		}
		catch(glare::Exception& e)
		{
			conPrint("Error: " + e.what());
		}
	}


	std::string server_hostname;
	int server_port;

	glare::TaskManager task_manager;

	ResourceManagerRef& resource_manager;

	ThreadManager resource_download_thread_manager;
	ThreadManager net_resource_download_thread_manager;
	ThreadManager resource_upload_thread_manager;

	ThreadSafeQueue<Reference<ThreadMessage> > msg_queue;

	glare::AtomicInt num_non_net_resources_downloading;
	glare::AtomicInt num_net_resources_downloading;

	Reference<ClientThread> client_thread;
};


int main(int argc, char* argv[])
{
	Clock::init();
	Networking::createInstance();
	PlatformUtils::ignoreUnixSignals();
	OpenSSL::init();
	TLSSocket::initTLS();

	ThreadSafeQueue<Reference<ThreadMessage> > msg_queue;

	Reference<WorldState> world_state = new WorldState();

	const std::string server_hostname = "localhost"; // "substrata.info"
	const int server_port = 7600;

	Reference<ClientThread> client_thread = new ClientThread(
		&msg_queue,
		server_hostname,
		server_port, // port
		"sdfsdf", // avatar URL
		"" // world name - default world
	);
	client_thread->world_state = world_state;

	ThreadManager client_thread_manager;
	client_thread_manager.addThread(client_thread);

	const std::string appdata_path = PlatformUtils::getOrCreateAppDataDirectory("Cyberspace");
	const std::string resources_dir = appdata_path + "/resources";
	conPrint("resources_dir: " + resources_dir);
	Reference<ResourceManager> resource_manager = new ResourceManager(resources_dir);


	// Make LogInMessage packet and enqueue to send
	{
		SocketBufferOutStream packet(SocketBufferOutStream::DontUseNetworkByteOrder);
		packet.writeUInt32(Protocol::LogInMessage);
		packet.writeStringLengthFirst(username);
		packet.writeStringLengthFirst(password);

		client_thread->enqueueDataToSend(packet);
	}

	// Wait until we have received parcel data.  This means we have received all objects
	conPrint("Waiting for initial data to be received");
	while(!client_thread->initial_state_received)
	{
		PlatformUtils::Sleep(10);
		conPrintStr(".");
	}

	conPrint("Received objects.  world_state->objects.size(): " + toString(world_state->objects.size()));

	conPrint("===================== Running LightMapperBot =====================");

	LightMapperBot bot(server_hostname, server_port, resource_manager);
	bot.doLightMapping(*world_state, client_thread);

	conPrint("===================== Done Running LightMapperBot. =====================");

	return 0;
}
