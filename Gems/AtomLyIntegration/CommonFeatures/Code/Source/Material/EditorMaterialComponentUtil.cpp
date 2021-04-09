/*
* All or portions of this file Copyright (c) Amazon.com, Inc. or its affiliates or
* its licensors.
*
* For complete copyright and license terms please see the LICENSE at the root of this
* distribution (the "License"). All use of this software is governed by the License,
* or, if provided, by the license below or the license accompanying this file. Do not
* remove or modify any license notices. This file is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
*
*/

#include <Material/EditorMaterialComponentUtil.h>

#include <Atom/RPI.Edit/Common/AssetUtils.h>
#include <Atom/RPI.Edit/Common/JsonUtils.h>
#include <Atom/RPI.Edit/Material/MaterialPropertyId.h>
#include <Atom/RPI.Edit/Material/MaterialSourceData.h>
#include <Atom/RPI.Edit/Material/MaterialTypeSourceData.h>
#include <Atom/RPI.Edit/Material/MaterialUtils.h>
#include <Atom/RPI.Reflect/Material/MaterialAsset.h>
#include <Atom/RPI.Reflect/Material/MaterialPropertiesLayout.h>
#include <Atom/RPI.Reflect/Material/MaterialTypeAsset.h>
#include <AzFramework/API/ApplicationAPI.h>
#include <AzToolsFramework/API/EditorAssetSystemAPI.h>
#include <AzToolsFramework/API/ToolsApplicationAPI.h>

namespace AZ
{
    namespace Render
    {
        namespace EditorMaterialComponentUtil
        {
            bool LoadMaterialEditDataFromAssetId(const AZ::Data::AssetId& assetId, MaterialEditData& editData)
            {
                editData = MaterialEditData();

                if (!assetId.IsValid())
                {
                    AZ_Warning("AZ::Render::EditorMaterialComponentUtil", false, "Attempted to load material data for invalid asset id.");
                    return false;
                }

                editData.m_materialAssetId = assetId;

                // Load the originating product asset from which the new source has set will be generated
                auto materialAssetOutcome = AZ::RPI::AssetUtils::LoadAsset<AZ::RPI::MaterialAsset>(editData.m_materialAssetId);
                if (!materialAssetOutcome)
                {
                    AZ_Error("AZ::Render::EditorMaterialComponentUtil", false, "Failed to load material asset: %s", editData.m_materialAssetId.ToString<AZStd::string>().c_str());
                    return false;
                }

                editData.m_materialAsset = materialAssetOutcome.GetValue();
                editData.m_materialTypeAsset = editData.m_materialAsset->GetMaterialTypeAsset();
                editData.m_materialParentAsset = {};

                editData.m_materialSourcePath = AZ::RPI::AssetUtils::GetSourcePathByAssetId(editData.m_materialAsset.GetId());
                if (AzFramework::StringFunc::Path::IsExtension(
                        editData.m_materialSourcePath.c_str(), AZ::RPI::MaterialSourceData::Extension))
                {
                    if (!AZ::RPI::JsonUtils::LoadObjectFromFile(editData.m_materialSourcePath, editData.m_materialSourceData))
                    {
                        AZ_Error("AZ::Render::EditorMaterialComponentUtil", false, "Material source data could not be loaded: '%s'.", editData.m_materialSourcePath.c_str());
                        return false;
                    }
                }

                if (!editData.m_materialSourceData.m_parentMaterial.empty())
                {
                    // There is a parent for this material
                    auto parentMaterialResult = AZ::RPI::AssetUtils::LoadAsset<AZ::RPI::MaterialAsset>(editData.m_materialSourcePath, editData.m_materialSourceData.m_parentMaterial);
                    if (!parentMaterialResult)
                    {
                        AZ_Error("AZ::Render::EditorMaterialComponentUtil", false, "Parent material asset could not be loaded: '%s'.", editData.m_materialSourceData.m_parentMaterial.c_str());
                        return false;
                    }

                    editData.m_materialParentAsset = parentMaterialResult.GetValue();
                    editData.m_materialParentSourcePath = AZ::RPI::AssetUtils::GetSourcePathByAssetId(editData.m_materialParentAsset.GetId());
                }

                // We need a valid path to the material type source data to get the property layout and assign to the new material
                editData.m_materialTypeSourcePath = AZ::RPI::AssetUtils::GetSourcePathByAssetId(editData.m_materialTypeAsset.GetId());
                if (editData.m_materialTypeSourcePath.empty())
                {
                    AZ_Error("AZ::Render::EditorMaterialComponentUtil", false, "Failed to locate source material type asset: %s", editData.m_materialAssetId.ToString<AZStd::string>().c_str());
                    return false;
                }

                // Load the material type source data
                auto materialTypeOutcome = AZ::RPI::MaterialUtils::LoadMaterialTypeSourceData(editData.m_materialTypeSourcePath);
                if (!materialTypeOutcome.IsSuccess())
                {
                    AZ_Error("AZ::Render::EditorMaterialComponentUtil", false, "Failed to load material type source data: %s", editData.m_materialTypeSourcePath.c_str());
                    return false;
                }
                editData.m_materialTypeSourceData = materialTypeOutcome.GetValue();
                return true;
            }

            bool SaveSourceMaterialFromEditData(const AZStd::string& path, const MaterialEditData& editData)
            {
                // Getting the source info for the material type file to make sure that it exists
                // We also need to watch folder to generate a relative asset path for the material type
                bool result = false;
                AZ::Data::AssetInfo info;
                AZStd::string watchFolder;
                AzToolsFramework::AssetSystemRequestBus::BroadcastResult(
                    result, &AzToolsFramework::AssetSystemRequestBus::Events::GetSourceInfoBySourcePath,
                    editData.m_materialTypeSourcePath.c_str(), info, watchFolder);
                if (!result)
                {
                    AZ_Error("AZ::Render::EditorMaterialComponentUtil", false, "Failed to get source file info and asset path while attempting to export: %s", path.c_str());
                    return false;
                }

                // Construct the material source data object that will be exported
                AZ::RPI::MaterialSourceData exportData;
                exportData.m_propertyLayoutVersion = editData.m_materialTypeSourceData.m_propertyLayout.m_version;

                // Converting absolute material paths to asset relative paths
                exportData.m_materialType = editData.m_materialTypeSourcePath;
                AzFramework::ApplicationRequests::Bus::Broadcast(
                    &AzFramework::ApplicationRequests::Bus::Events::MakePathRelative, exportData.m_materialType, watchFolder.c_str());

                exportData.m_parentMaterial = editData.m_materialParentSourcePath;
                AzFramework::ApplicationRequests::Bus::Broadcast(
                    &AzFramework::ApplicationRequests::Bus::Events::MakePathRelative, exportData.m_parentMaterial, watchFolder.c_str());

                // Copy all of the properties from the material asset to the source data that will be exported
                result = true;
                editData.m_materialTypeSourceData.EnumerateProperties([&](const AZStd::string& groupNameId, const AZStd::string& propertyNameId, const auto& propertyDefinition) {
                    const AZ::RPI::MaterialPropertyId propertyId(groupNameId, propertyNameId);
                    const AZ::RPI::MaterialPropertyIndex propertyIndex =
                        editData.m_materialAsset->GetMaterialPropertiesLayout()->FindPropertyIndex(propertyId.GetFullName());

                    AZ::RPI::MaterialPropertyValue propertyValue = editData.m_materialAsset->GetPropertyValues()[propertyIndex.GetIndex()];

                    AZ::RPI::MaterialPropertyValue propertyValueDefault = propertyDefinition.m_value;
                    if (editData.m_materialParentAsset.IsReady())
                    {
                        propertyValueDefault = editData.m_materialParentAsset->GetPropertyValues()[propertyIndex.GetIndex()];
                    }

                    // Check for and apply any property overrides before saving property values
                    auto propertyOverrideItr = editData.m_materialPropertyOverrideMap.find(propertyId.GetFullName());
                    if(propertyOverrideItr != editData.m_materialPropertyOverrideMap.end())
                    {
                        propertyValue = AZ::RPI::MaterialPropertyValue::FromAny(propertyOverrideItr->second);
                    }

                    if (!editData.m_materialTypeSourceData.ConvertPropertyValueToSourceDataFormat(propertyDefinition, propertyValue))
                    {
                        AZ_Error("AZ::Render::EditorMaterialComponentUtil", false, "Failed to export: %s", path.c_str());
                        result = false;
                        return false;
                    }

                    // Don't export values if they are the same as the material type or parent
                    if (propertyValueDefault == propertyValue)
                    {
                        return true;
                    }

                    exportData.m_properties[groupNameId][propertyDefinition.m_nameId].m_value = propertyValue;
                    return true;
                });

                return result && AZ::RPI::JsonUtils::SaveObjectToFile(path, exportData);
            }
        } // namespace EditorMaterialComponentUtil
    } // namespace Render
} // namespace AZ

//#include <AtomLyIntegration/CommonFeatures/moc_EditorMaterialComponentUtil.cpp>