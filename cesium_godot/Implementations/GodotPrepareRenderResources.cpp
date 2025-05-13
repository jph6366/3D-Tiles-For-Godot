#include "GodotPrepareRenderResources.h"
#include "CesiumGltf/ImageAsset.h"
#include "CesiumGltfReader/ImageDecoder.h"
#include "Models/Cesium3DTile.h"
#include "Models/CesiumGlobe.h"
#include "Utils/CesiumVariantHash.h"
#include "error_names.hpp"
#include "glm/ext/vector_double3.hpp"
#include "glm/fwd.hpp"

#if defined(CESIUM_GD_EXT)
#include <godot_cpp/classes/mesh_instance3d.hpp>
#elif defined(CESIUM_GD_MODULE)
#include "scene/3d/mesh_instance_3d.h"
using namespace godot;
#endif

#include "CesiumAsync/AsyncSystem.h"
#include "Cesium3DTilesSelection/Tile.h"
#include "../CesiumGDModelLoader.h"
#include "../Utils/CesiumGDTextureLoader.h"
#include "CesiumRasterOverlays/RasterOverlayTile.h"
#include "CesiumRasterOverlays/RasterOverlay.h"
#include "../Utils/CesiumMathUtils.h"
#include "CesiumGltf/Node.h"
#include "CesiumGltf/ExtensionModelExtStructuralMetadata.h"
#include <glm/gtc/quaternion.hpp>
#include "../Models/CesiumGDTileset.h"

using namespace CesiumAsync;
using namespace Cesium3DTilesSelection;

CesiumAsync::Future<Cesium3DTilesSelection::TileLoadResultAndRenderResources> GodotPrepareRenderResources::prepareInLoadThread(const CesiumAsync::AsyncSystem& asyncSystem, Cesium3DTilesSelection::TileLoadResult&& tileLoadResult, const glm::dmat4& transform, const std::any& rendererOptions)
{
	CesiumGltf::Model* model = std::get_if<CesiumGltf::Model>(&tileLoadResult.contentKind);

	if (model == nullptr) {
		return asyncSystem.createResolvedFuture(TileLoadResultAndRenderResources{ std::move(tileLoadResult), nullptr });
	}

	return asyncSystem.createFuture<TileLoadResultAndRenderResources>([=, this](Promise<TileLoadResultAndRenderResources> p_promise) {
		Error err;
		Ref<ArrayMesh> meshData = CesiumGDModelLoader::generate_meshes_from_model(*model, &err);

		Cesium3DTile* instance = memnew(Cesium3DTile);
		instance->set_mesh(meshData);
		
		if (err != Error::OK) {
			std::string errorMsg = std::string("Error generating meshes for tile ") + REFLECT_ERR_NAME(err);
			std::exception exc(errorMsg.c_str());
			p_promise.reject(&exc);
		}

		const CesiumGltf::Node &rootNode = model->nodes.at(0);

		const std::vector<double> &rotationArray = rootNode.rotation;
		const std::vector<double> &scaleArray = rootNode.scale;

		Transform3D gdTransform = Transform3D(Basis(), Vector3());
		gdTransform.scale(Vector3(1, 1, 1));
		instance->set_transform(gdTransform);

		Vector3 scale;
		scale.x = scaleArray.at(0);
		scale.y = scaleArray.at(1);
		scale.z = scaleArray.at(2);

		const glm::dmat4 transformationMat = CesiumMathUtils::array_to_dmat4(rootNode.matrix);

		glm::dvec3 glmPos;
		glm::dquat glmRot;
		// Applies for tilesets that 
		constexpr int32_t worldTerrainId = 1;
		constexpr int32_t osmBuildingsId = 96188;
		// Applies for osmBuildings and world terrain
		int32_t currAssetId = this->m_tileset->get_ion_asset_id();
		if (this->m_tileset->get_data_source() == CesiumDataSource::FromCesiumIon && (currAssetId == worldTerrainId || currAssetId == osmBuildingsId)) {
			constexpr int32_t translationColumnIndex = 3;
			glmPos = transformationMat[translationColumnIndex];
			glmRot = glm::quat_cast(transformationMat);
		}
		else {
			const std::vector<double> &translationArray = rootNode.translation;
			const std::vector<double> &rotationArray = rootNode.rotation;
			glmPos = *reinterpret_cast<const glm::dvec3*>(translationArray.data());
			glmRot = *reinterpret_cast<const glm::dquat*>(rotationArray.data());
		}

		Vector3 translation;
		Quaternion rotation = CesiumMathUtils::from_glm_quat(glmRot);
		CesiumGeoreference* geoReferenceNode = nullptr;

		if (this->m_tileset->is_georeferenced(&geoReferenceNode)) {
			real_t scaleFactor = geoReferenceNode->get_scale_factor();
			instance->set_scale({ scaleFactor, scaleFactor, scaleFactor });
			translation *= scaleFactor;
		}

		if (geoReferenceNode->get_origin_type() == (int32_t)CesiumGeoreference::OriginType::CartographicOrigin) {
			// Save this for use later
			instance->set_original_position(glmPos);
		}

		Vector3 eulerAngles = rotation.get_euler();

		translation = CesiumMathUtils::from_glm_vec3(glmPos);
		instance->set_position(translation);
		instance->set_rotation(eulerAngles);
		if (this->m_tileset->get_create_physics_meshes()) {
			instance->generate_tile_collision();
		}

		// Metadata extraction
		const CesiumGltf::ExtensionModelExtStructuralMetadata* modelMetadata = model->getExtension<CesiumGltf::ExtensionModelExtStructuralMetadata>();
		instance->add_metadata(model, modelMetadata);

		TileLoadResultAndRenderResources result{
			std::move(tileLoadResult),
			static_cast<void*>(instance)
		};

		p_promise.resolve(result);

	});
}

void* GodotPrepareRenderResources::prepareInMainThread(Tile& tile, void* pLoadThreadResult)
{
	const Cesium3DTilesSelection::TileContent& content = tile.getContent();
	const Cesium3DTilesSelection::TileRenderContent* pRenderContent = content.getRenderContent();
	
	if (pRenderContent == nullptr) {
		return pLoadThreadResult;
	}
	
	const CesiumGltf::Model& model = pRenderContent->getModel();
	// Apply the transform if any
	Cesium3DTile* instance = reinterpret_cast<Cesium3DTile*>(pLoadThreadResult);
	// glm::dmat4 instanceXform = CesiumMathUtils::to_glm_mat4(instance->get_transform());
	glm::dmat4 tileTransform = tile.getTransform();
	tileTransform = CesiumGDModelLoader::apply_rtc_center(model, tileTransform);
	tileTransform = CesiumGDModelLoader::apply_gltf_up_axis_transform(model, tileTransform);
	// Then apply the z up
	glm::dvec3 position = glm::dvec3(CesiumMathUtils::ecef_to_engine(tileTransform[3]));
	instance->set_original_position(instance->get_original_position() + position);
	return pLoadThreadResult;
}

void GodotPrepareRenderResources::free(Tile& tile, void* pLoadThreadResult, void* pMainThreadResult) noexcept
{	
	const auto& tileId = tile.getTileID();
	size_t hash = std::visit(CesiumVariantHash{}, tileId);
	auto* instance = static_cast<Cesium3DTile*>(pMainThreadResult);
	this->m_tileset->call_deferred("free_tile", instance, hash);
}

void GodotPrepareRenderResources::attachRasterInMainThread(const Tile& tile, int32_t overlayTextureCoordinateID, const CesiumRasterOverlays::RasterOverlayTile& rasterTile, void* pMainThreadRendererResources, const glm::dvec2& translation, const glm::dvec2& scale)
{
	const Cesium3DTilesSelection::TileContent& content = tile.getContent();
	void* rawRenderResources = content.getRenderContent()->getRenderResources();
	auto* meshInstance = static_cast<Cesium3DTile*>(rawRenderResources);
	if (meshInstance == nullptr) return;

	Ref<ImageTexture> godotTexture = static_cast<ImageTexture*>(pMainThreadRendererResources);
	std::string key = rasterTile.getOverlay().getName();

	//Get all primitives (surfaces) in the mesh tile
	uint32_t primitiveIndex = 0;
	const CesiumGltf::Model& model = content.getRenderContent()->getModel();
	std::string overlayAttributeName = "_CESIUMOVERLAY_" + std::to_string(overlayTextureCoordinateID);

	Ref<ArrayMesh> arrayMesh = meshInstance->get_mesh();

	const Vector2 scaleFactors = CesiumMathUtils::from_glm_vec2(scale);
	const Vector2 translationOffsets = CesiumMathUtils::from_glm_vec2(translation);
	
	for (const CesiumGltf::Mesh& mesh : model.meshes) {
		for (const CesiumGltf::MeshPrimitive& primitive : mesh.primitives) {
			std::_List_const_iterator attributeIt = primitive.attributes.find(overlayAttributeName);
			if (attributeIt == primitive.attributes.end()) {
				continue;
			}

			Array arrays = arrayMesh->surface_get_arrays(primitiveIndex);

			int32_t uvIndex = attributeIt->second;

			Ref<StandardMaterial3D> godotMaterial = memnew(StandardMaterial3D);
			const CesiumGltf::Material& mat = model.materials.at(primitive.material);
			Error err = CesiumGDModelLoader::copy_material_properties(mat, godotMaterial, model);
			godotMaterial->set_texture(BaseMaterial3D::TEXTURE_ALBEDO, godotTexture);
			godotMaterial->set_uv1_scale(Vector3(scale.x, -scale.y, 1.0));
			godotMaterial->set_uv1_offset(Vector3(translation.x, 1 - translation.y, 1.0));
			if (err != Error::OK) {
				ERR_PRINT(String("Could not set texture for Raster Overlay, error: ") + REFLECT_ERR_NAME(err));
				continue;
			}

			//Apply the texture to the surface
			err = CesiumGDModelLoader::apply_surface_to_mesh(primitive, arrayMesh, arrays);
			if (err != Error::OK) {
				ERR_PRINT(String("Could not set texture for Raster Overlay, error: ") + REFLECT_ERR_NAME(err));
				continue;
			}

			arrayMesh->surface_set_material(primitiveIndex, godotMaterial);

			primitiveIndex++;
		}
	}
	meshInstance->set_mesh(arrayMesh);
}

void GodotPrepareRenderResources::detachRasterInMainThread(const Tile& tile, int32_t overlayTextureCoordinateID, const CesiumRasterOverlays::RasterOverlayTile& rasterTile, void* pMainThreadRendererResources) noexcept
{

}

void* GodotPrepareRenderResources::prepareRasterInLoadThread(CesiumGltf::ImageAsset& image, const std::any& rendererOptions)
{
	//I guess we just generate mip maps here (?
	CesiumGltfReader::ImageDecoder::generateMipMaps(image);
	return nullptr;
}

void* GodotPrepareRenderResources::prepareRasterInMainThread(CesiumRasterOverlays::RasterOverlayTile& rasterTile, void* pLoadThreadResult)
{
	const CesiumGltf::ImageAsset& imageCesium = *rasterTile.getImage().get();
	Ref<ImageTexture> godotTexture = CesiumGDTextureLoader::load_image_texture(imageCesium, false, true);
	godotTexture->reference();
	return static_cast<void*>(godotTexture.ptr());
}

void GodotPrepareRenderResources::freeRaster(const CesiumRasterOverlays::RasterOverlayTile& rasterTile, void* pLoadThreadResult, void* pMainThreadResult) noexcept
{
	auto* rasterMainThreadTexture = static_cast<ImageTexture*>(pMainThreadResult);
	if (rasterMainThreadTexture == nullptr) return;
	rasterMainThreadTexture->unreference();
}
