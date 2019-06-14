/*=========================================================================
 *
 *  Copyright SINAPSE: Scalable Informatics for Neuroscience, Processing and Software Engineering
 *            The University of Iowa
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *         http://www.apache.org/licenses/LICENSE-2.0.txt
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 *
 *=========================================================================*/
/*
 * Author: Han J. Johnson, Wei Lu
 * at Psychiatry Imaging Lab,
 * University of Iowa Health Care 2010
 */

#include "landmarksConstellationDetector.h"
// landmarkIO has to be included after landmarksConstellationDetector
#include "landmarkIO.h"
#include "itkOrthogonalize3DRotationMatrix.h"

#include "itkFindCenterOfBrainFilter.h"
#include "BRAINSHoughEyeDetector.h"

#include <BRAINSFitHelper.h>
#include "itkLandmarkBasedTransformInitializer.h"

std::string
local_to_string( unsigned int i )
{
  std::stringstream localStream;
  localStream << i;
  return localStream.str();
}

// NOTE: LandmarkTransforms are inverse of ImageTransforms, (You pull images, you push landmarks)
static VersorTransformType::Pointer
GetLandmarkTransformFromImageTransform( VersorTransformType::ConstPointer orig2msp_img_tfm )
{
  VersorTransformType::Pointer orig2msp_lmk_tfm = VersorTransformType::New();
  SImageType::PointType        centerPoint = orig2msp_img_tfm->GetCenter();
  orig2msp_lmk_tfm->SetCenter( centerPoint );
  orig2msp_lmk_tfm->SetIdentity();
  orig2msp_img_tfm->GetInverse( orig2msp_lmk_tfm );
  return orig2msp_lmk_tfm;
}

VersorTransformType::Pointer
landmarksConstellationDetector::Compute_orig2msp_img_tfm( const LandmarksMapType & updated_lmks )
{
  SImageType::PointType ZeroCenter;
  ZeroCenter.Fill( 0.0 );

  RigidTransformType::Pointer orig2msp_lmk_tfm_estimate =
    computeTmspFromPoints( updated_lmks.at( "RP" ), updated_lmks.at( "AC" ), updated_lmks.at( "PC" ), ZeroCenter );

  VersorTransformType::Pointer orig2msp_lmk_tfm_cleaned = VersorTransformType::New();
  orig2msp_lmk_tfm_cleaned->SetFixedParameters( orig2msp_lmk_tfm_estimate->GetFixedParameters() );

  itk::Versor< double >               versorRotation;
  const itk::Matrix< double, 3, 3 > & CleanedOrthogonalized =
    itk::Orthogonalize3DRotationMatrix( orig2msp_lmk_tfm_estimate->GetMatrix() );
  versorRotation.Set( CleanedOrthogonalized );

  orig2msp_lmk_tfm_cleaned->SetRotation( versorRotation );
  orig2msp_lmk_tfm_cleaned->SetTranslation( orig2msp_lmk_tfm_estimate->GetTranslation() );
  return orig2msp_lmk_tfm_cleaned;
}

void
landmarksConstellationDetector::ComputeFinalRefinedACPCAlignedTransform( SImageType::Pointer original_space_image )
{
  ////////////////////////////
  // START BRAINSFit alternative
  if ( !this->m_atlasVolume.empty() )
  {
    using AtlasReaderType = itk::ImageFileReader< SImageType >;
    AtlasReaderType::Pointer atlasReader = AtlasReaderType::New();
    atlasReader->SetFileName( this->m_atlasVolume );
    std::cout << "read atlas: " << this->m_atlasVolume << std::endl;
    try
    {
      atlasReader->Update();
    }
    catch ( itk::ExceptionObject & err )
    {
      std::cerr << "Error while reading atlasVolume file:\n " << err << std::endl;
    }

    // TODO: prob needs a try-catch
    std::cout << "read atlas landmarks:  " << this->m_atlasLandmarks << std::endl;
    LandmarksMapType referenceAtlasLandmarks = ReadSlicer3toITKLmk( this->m_atlasLandmarks );

    // Create a better version of this->m_orig2msp_img_tfm using BRAINSFit.
    // take the the subjects landmarks in original space, and  landmarks from a reference Atlas, and compute an initial
    // affine transform
    // ( using logic from BRAINSLandmarkInitializer) and create initToAtlasAffineTransform.

    using WeightType = std::map< std::string, float >;
    WeightType landmarkWeights;
    if ( this->m_atlasLandmarkWeights != "" )
    {
      landmarkWeights = ReadLandmarkWeights( this->m_atlasLandmarkWeights.c_str() );
      std::cout << "read atlas landmarksweights:  " << this->m_atlasLandmarkWeights << std::endl;
    }
    // TEST turning this back on.
    using LmkInitTransformType = itk::AffineTransform< double, Dimension >;
    using LandmarkBasedInitializerType =
      itk::LandmarkBasedTransformInitializer< LmkInitTransformType, SImageType, SImageType >;
    using LandmarkContainerType = LandmarkBasedInitializerType::LandmarkPointContainer;
    LandmarkContainerType atlasLmks;
    LandmarkContainerType movingLmks;
    using LandmarkConstIterator = LandmarksMapType::const_iterator;
    LandmarkBasedInitializerType::LandmarkWeightType landmarkWgts;
    for ( LandmarkConstIterator fixedIt = referenceAtlasLandmarks.begin(); fixedIt != referenceAtlasLandmarks.end();
          ++fixedIt )
    {
      LandmarkConstIterator movingIt = this->m_orig_lmks_constant.find( fixedIt->first );
      if ( movingIt != this->m_orig_lmks_constant.end() )
      {
        atlasLmks.push_back( fixedIt->second );
        movingLmks.push_back( movingIt->second );
        if ( !this->m_atlasLandmarkWeights.empty() )
        {
          if ( landmarkWeights.find( fixedIt->first ) != landmarkWeights.end() )
          {
            landmarkWgts.push_back( landmarkWeights[fixedIt->first] );
          }
          else
          {
            std::cout << "Landmark for " << fixedIt->first << " does not exist. "
                      << "Set the weight to 0.5 " << std::endl;
            landmarkWgts.push_back( 0.5F );
          }
        }
      }
      else
      {
        itkGenericExceptionMacro( << "Could not find " << fixedIt->first << " in originalSpaceLandmarksPreBRAINSFit "
                                  << std::endl
                                  << "MIS MATCHED MOVING AND FIXED LANDMARKS!" << std::endl );
      }
    }

    LandmarkBasedInitializerType::Pointer landmarkBasedInitializer = LandmarkBasedInitializerType::New();

    if ( !this->m_atlasLandmarkWeights.empty() )
    {
      landmarkBasedInitializer->SetLandmarkWeight( landmarkWgts );
    }
    landmarkBasedInitializer->SetFixedLandmarks( atlasLmks );
    landmarkBasedInitializer->SetMovingLandmarks( movingLmks );

    LmkInitTransformType::Pointer initToAtlasAffineTransform = LmkInitTransformType::New();
    landmarkBasedInitializer->SetTransform( initToAtlasAffineTransform );
    landmarkBasedInitializer->InitializeTransform();

    using HelperType = itk::BRAINSFitHelper;
    HelperType::Pointer brainsFitHelper = HelperType::New();

    // Now Run BRAINSFitHelper class initialized with initToAtlasAffineTransform, original image, and atlas image
    // adapted from BRAINSABC/brainseg/AtlasRegistrationMethod.hxx - do I need to change any of these parameters?
    brainsFitHelper->SetSamplingPercentage( 0.05 ); // Use 5% of voxels for samples
    brainsFitHelper->SetNumberOfHistogramBins( 50 );
    const std::vector< int > numberOfIterations( 1, 1500 );
    brainsFitHelper->SetNumberOfIterations( numberOfIterations );
    brainsFitHelper->SetTranslationScale( 1000 );
    brainsFitHelper->SetReproportionScale( 1.0 );
    brainsFitHelper->SetSkewScale( 1.0 );

    using FloatImageType = itk::Image< float, 3 >;
    using CastFilterType = itk::CastImageFilter< SImageType, FloatImageType >;

    {
      CastFilterType::Pointer fixedCastFilter = CastFilterType::New();
      fixedCastFilter->SetInput( atlasReader->GetOutput() );
      fixedCastFilter->Update();
      brainsFitHelper->SetFixedVolume( fixedCastFilter->GetOutput() );

      CastFilterType::Pointer movingCastFilter = CastFilterType::New();
      movingCastFilter->SetInput( original_space_image );
      movingCastFilter->Update();
      brainsFitHelper->SetMovingVolume( movingCastFilter->GetOutput() );
      itkUtil::WriteImage< FloatImageType >( movingCastFilter->GetOutput(), "./DEBUGFloatCastMovingImage.nii.gz" );
    }

    const std::vector< double > minimumStepSize( 1, 0.0005 );
    brainsFitHelper->SetMinimumStepLength( minimumStepSize );
    std::vector< std::string > transformType( 1 );
    transformType[0] = "Affine";
    brainsFitHelper->SetTransformType( transformType );

    using CompositeTransformType = itk::CompositeTransform< double, 3 >;
    CompositeTransformType::Pointer initToAtlasAffineCompositeTransform =
      dynamic_cast< CompositeTransformType * >( initToAtlasAffineTransform.GetPointer() );
    if ( initToAtlasAffineCompositeTransform.IsNull() )
    {
      initToAtlasAffineCompositeTransform = CompositeTransformType::New();
      initToAtlasAffineCompositeTransform->AddTransform( initToAtlasAffineTransform );
    }
    brainsFitHelper->SetCurrentGenericTransform( initToAtlasAffineCompositeTransform );

    // Provide better masking of images
    {
      // if( fixedBinaryVolume != "" || movingBinaryVolume != "" )
      //  {
      //  std::cout
      //    << "ERROR:  Can not specify mask file names when ROIAUTO is used for the maskProcessingMode"
      //    << std::endl;
      //  return EXIT_FAILURE;
      //  }
      static constexpr unsigned int ROIAutoClosingSize = 4;
      static constexpr unsigned int ROIAutoDilateSize = 6;
      {
        using ROIAutoType = itk::BRAINSROIAutoImageFilter< SImageType, itk::Image< unsigned char, 3 > >;
        ROIAutoType::Pointer ROIFilter = ROIAutoType::New();
        ROIFilter->SetInput( atlasReader->GetOutput() );
        ROIFilter->SetClosingSize( ROIAutoClosingSize );
        ROIFilter->SetDilateSize( ROIAutoDilateSize );
        ROIFilter->Update();
        ImageMaskPointer fixedMask = ROIFilter->GetSpatialObjectROI();
        brainsFitHelper->SetFixedBinaryVolume( fixedMask );
      }
      {
        using ROIAutoType = itk::BRAINSROIAutoImageFilter< SImageType, itk::Image< unsigned char, 3 > >;
        ROIAutoType::Pointer ROIFilter = ROIAutoType::New();
        ROIFilter->SetInput( original_space_image );
        ROIFilter->SetClosingSize( ROIAutoClosingSize );
        ROIFilter->SetDilateSize( ROIAutoDilateSize );
        ROIFilter->Update();
        ImageMaskPointer movingMask = ROIFilter->GetSpatialObjectROI();
        brainsFitHelper->SetMovingBinaryVolume( movingMask );
      }
    }
    brainsFitHelper->SetDebugLevel( 10 );
    brainsFitHelper->Update();

    this->m_orig2msp_img_tfm = itk::ComputeRigidTransformFromGeneric(
      brainsFitHelper->GetCurrentGenericTransform()->GetNthTransform( 0 ).GetPointer() );
    if ( this->m_orig2msp_img_tfm.IsNull() )
    {
      // Fail if something weird happens.
      itkGenericExceptionMacro( << "this->m_orig2msp_img_tfm is null. "
                                << "It means we're not registering to the atlas, after all." << std::endl );
    }

    {
      // NOTE: LandmarkTransforms are inverse of ImageTransforms, (You pull images, you push landmarks)
      using VersorRigid3DTransformType = itk::VersorRigid3DTransform< double >;
      VersorTransformType::Pointer LandmarkOrigToACPCTransform =
        GetLandmarkTransformFromImageTransform( this->m_orig2msp_img_tfm.GetPointer() );

      const VersorRigid3DTransformType::OutputPointType acPointInACPCSpace =
        LandmarkOrigToACPCTransform->TransformPoint(
          GetNamedPointFromLandmarkList( this->m_orig_lmks_constant, "AC" ) );
      // std::cout << "HACK: PRE-FIXING" << acPointInACPCSpace << std::endl;
      {
        VersorRigid3DTransformType::OffsetType translation;
        translation[0] =
          +acPointInACPCSpace[0]; // NOTE: Positive translation in ImageTransform is Negative in LandmarkTransform
        translation[1] = +acPointInACPCSpace[1];
        translation[2] = +acPointInACPCSpace[2];
        // First shift the transform
        this->m_orig2msp_img_tfm->Translate( translation, false );
      }

      // LandmarkOrigToACPCTransform = GetLandmarkTransformFromImageTransform( this->m_orig2msp_img_tfm.GetPointer()  );
      // VersorRigid3DTransformType::OutputPointType newacPointInACPCSpace =
      // LandmarkOrigToACPCTransform->TransformPoint(GetNamedPointFromLandmarkList(this->m_orig_lmks_constant,"AC"));
      // std::cout << "HACK: POST-FIXING" << newacPointInACPCSpace << std::endl;
      // TODO:  This still does not put it to (0,0,0) and it should.
    }
  }
  /// END BRAINSFIT_ALTERNATIVE
  ////////////////////////////
}

VersorTransformType::Pointer
landmarksConstellationDetector::GetImageOrigToACPCVersorTransform( void ) const
{
  return m_orig2msp_img_tfm;
}

SImageType::PointType
landmarksConstellationDetector::FindCandidatePoints(
  SImageType::Pointer volumeMSP, SImageType::Pointer mask_LR, const double LR_restrictions,
  const double PA_restrictions, const double SI_restrictions,
  // TODO: restrictions should really be ellipsoidal values
  const SImageType::PointType::VectorType &                      CenterOfSearchArea,
  const std::vector< std::vector< float > > &                    TemplateMean,
  const landmarksConstellationModelIO::IndexLocationVectorType & model, double & cc_Max, const std::string & mapID )
{
  cc_Max = -123456789.0;

  LinearInterpolatorType::Pointer imInterp = LinearInterpolatorType::New();
  imInterp->SetInputImage( volumeMSP );
  LinearInterpolatorType::Pointer maskInterp = LinearInterpolatorType::New();
  maskInterp->SetInputImage( mask_LR );

  // Final location is initialized with the center of search area
  SImageType::PointType GuessPoint;
  GuessPoint[0] = CenterOfSearchArea[0];
  GuessPoint[1] = CenterOfSearchArea[1];
  GuessPoint[2] = CenterOfSearchArea[2];

  // Boundary check
  {
    SImageType::PointType boundaryL( GuessPoint );
    boundaryL[0] += LR_restrictions;
    SImageType::PointType boundaryR( GuessPoint );
    boundaryR[0] -= LR_restrictions;
    SImageType::PointType boundaryP( GuessPoint );
    boundaryP[1] += PA_restrictions;
    SImageType::PointType boundaryA( GuessPoint );
    boundaryA[1] -= PA_restrictions;
    SImageType::PointType boundaryS( GuessPoint );
    boundaryS[2] += SI_restrictions;
    SImageType::PointType boundaryI( GuessPoint );
    boundaryI[2] -= SI_restrictions;
    if ( ( !maskInterp->IsInsideBuffer( boundaryL ) ) || ( !maskInterp->IsInsideBuffer( boundaryR ) ) ||
         ( !maskInterp->IsInsideBuffer( boundaryP ) ) || ( !maskInterp->IsInsideBuffer( boundaryA ) ) ||
         ( !maskInterp->IsInsideBuffer( boundaryS ) ) || ( !maskInterp->IsInsideBuffer( boundaryI ) ) )
    {
      std::cout << "WARNING: search region outside of the image region." << std::endl;
      std::cout << "The detection has probably large error!" << std::endl;
      return GuessPoint;
    }
  }

  // height and radius of the moving template
  const double height = this->m_InputTemplateModel.GetHeight( mapID );
  double       radii = this->m_InputTemplateModel.GetRadius( mapID );

  // HACK:
  // When, rmpj, rac, rpc and rvn4 are used, they help to increase the bounding box
  // restrictions; however, we do not want that landmark template image be affected
  // by that.
  if ( mapID == "RP" || mapID == "AC" || mapID == "PC" || mapID == "VN4" )
  {
    if ( radii > 5 )
    {
      radii = 5;
    }
  }

  // Bounding box around the center point.
  // To compute the correlation for the border points, the bounding box needs
  // to be expanded by the template size.
  //
  const double LeftToRight_BEGIN = CenterOfSearchArea[0] - LR_restrictions - height / 2;
  const double AnteriorToPostierior_BEGIN = CenterOfSearchArea[1] - PA_restrictions - radii;
  const double InferiorToSuperior_BEGIN = CenterOfSearchArea[2] - SI_restrictions - radii;

  const double LeftToRight_END = CenterOfSearchArea[0] + LR_restrictions + height / 2;
  const double AnteriorToPostierior_END = CenterOfSearchArea[1] + PA_restrictions + radii;
  const double InferiorToSuperior_END = CenterOfSearchArea[2] + SI_restrictions + radii;

  // Now bounding area will be converted to an image
  //
  SImageType::Pointer roiImage = SImageType::New();
  // origin
  SImageType::PointType roiOrigin;
  roiOrigin[0] = LeftToRight_BEGIN;
  roiOrigin[1] = AnteriorToPostierior_BEGIN;
  roiOrigin[2] = InferiorToSuperior_BEGIN;
  roiImage->SetOrigin( roiOrigin );
  // size
  SImageType::SizeType roiSize;
  roiSize[0] = ( LeftToRight_END - LeftToRight_BEGIN ) / ( volumeMSP->GetSpacing()[0] ) + 1;
  roiSize[1] = ( AnteriorToPostierior_END - AnteriorToPostierior_BEGIN ) / ( volumeMSP->GetSpacing()[1] ) + 1;
  roiSize[2] = ( InferiorToSuperior_END - InferiorToSuperior_BEGIN ) / ( volumeMSP->GetSpacing()[2] ) + 1;
  // start index
  SImageType::IndexType roiStart;
  roiStart.Fill( 0 );
  // region
  SImageType::RegionType roiRegion( roiStart, roiSize );
  roiImage->SetRegions( roiRegion );
  // spacing
  roiImage->SetSpacing( volumeMSP->GetSpacing() );
  // direction
  roiImage->SetDirection( volumeMSP->GetDirection() );
  roiImage->Allocate();
  roiImage->FillBuffer( 0 );

  // Since the actual bounding box is a rounded area, a Roi mask is also needed.
  //
  SImageType::Pointer roiMask = SImageType::New();
  roiMask->CopyInformation( roiImage );
  roiMask->SetRegions( roiImage->GetLargestPossibleRegion() );
  roiMask->Allocate();
  roiMask->FillBuffer( 0 );

  // roiImage is filled with values from volumeMSP
  //
  SImageType::PointType currentPointLocation;
  currentPointLocation[0] = CenterOfSearchArea[0];
  currentPointLocation[1] = CenterOfSearchArea[1];
  currentPointLocation[2] = CenterOfSearchArea[2];

  constexpr double deltaLR = 1; // in mm
  constexpr double deltaAP = 1; // in mm
  constexpr double deltaIS = 1; // in mm

  for ( double LeftToRight = LeftToRight_BEGIN; LeftToRight < LeftToRight_END; LeftToRight += deltaLR )
  {
    currentPointLocation[0] = LeftToRight;
    for ( double AnteriorToPostierior = AnteriorToPostierior_BEGIN; AnteriorToPostierior < AnteriorToPostierior_END;
          AnteriorToPostierior += deltaAP )
    {
      currentPointLocation[1] = AnteriorToPostierior;
      for ( double InferiorToSuperior = InferiorToSuperior_BEGIN; InferiorToSuperior < InferiorToSuperior_END;
            InferiorToSuperior += deltaIS )
      {
        currentPointLocation[2] = InferiorToSuperior;

        // Is current point within the input mask
        if ( maskInterp->Evaluate( currentPointLocation ) > 0.5 )
        {
          // Is current point inside the boundary box
          const SImageType::PointType::VectorType temp =
            currentPointLocation.GetVectorFromOrigin() - CenterOfSearchArea;
          const double inclusionDistance = temp.GetNorm();
          if ( ( inclusionDistance < ( SI_restrictions + radii ) ) &&
               ( std::abs( temp[1] ) < ( PA_restrictions + radii ) ) )
          {
            SImageType::IndexType index3D;
            roiImage->TransformPhysicalPointToIndex( currentPointLocation, index3D );
            roiImage->SetPixel( index3D, imInterp->Evaluate( currentPointLocation ) );
            roiMask->SetPixel( index3D, 1 );
          }
        }
      }
    }
  }

  ////////
  // Now we need to normalize only bounding region inside the roiImage
  ///////
  using BinaryImageToLabelMapFilterType = itk::BinaryImageToLabelMapFilter< SImageType >;
  BinaryImageToLabelMapFilterType::Pointer binaryImageToLabelMapFilter = BinaryImageToLabelMapFilterType::New();
  binaryImageToLabelMapFilter->SetInput( roiMask );
  binaryImageToLabelMapFilter->Update();

  using LabelMapToLabelImageFilterType =
    itk::LabelMapToLabelImageFilter< BinaryImageToLabelMapFilterType::OutputImageType, SImageType >;
  LabelMapToLabelImageFilterType::Pointer labelMapToLabelImageFilter = LabelMapToLabelImageFilterType::New();
  labelMapToLabelImageFilter->SetInput( binaryImageToLabelMapFilter->GetOutput() );
  labelMapToLabelImageFilter->Update();

  using LabelStatisticsImageFilterType = itk::LabelStatisticsImageFilter< SImageType, SImageType >;
  LabelStatisticsImageFilterType::Pointer labelStatisticsImageFilter = LabelStatisticsImageFilterType::New();
  labelStatisticsImageFilter->SetLabelInput( labelMapToLabelImageFilter->GetOutput() );
  labelStatisticsImageFilter->SetInput( roiImage );
  labelStatisticsImageFilter->Update();

  if ( labelStatisticsImageFilter->GetNumberOfLabels() != 1 )
  {
    itkGenericExceptionMacro( << "The bounding box mask should be connected." );
  }

  using LabelPixelType = LabelStatisticsImageFilterType::LabelPixelType;
  LabelPixelType      labelValue = labelStatisticsImageFilter->GetValidLabelValues()[0];
  const double        ROImean = labelStatisticsImageFilter->GetMean( labelValue );
  const double        ROIvar = labelStatisticsImageFilter->GetVariance( labelValue );
  const unsigned long ROIcount = labelStatisticsImageFilter->GetCount( labelValue );

  // The area inside the bounding box is normalized using the mean and variance statistics
  using SubtractImageFilterType = itk::SubtractImageFilter< SImageType, SImageType, FImageType3D >;
  SubtractImageFilterType::Pointer subtractConstantFromImageFilter = SubtractImageFilterType::New();
  subtractConstantFromImageFilter->SetInput( roiImage );
  subtractConstantFromImageFilter->SetConstant2( ROImean );
  subtractConstantFromImageFilter->Update();

  if ( std::sqrt( ROIcount * ROIvar ) < std::numeric_limits< double >::epsilon() )
  {
    itkGenericExceptionMacro( << "Zero norm for bounding area." );
  }
  const double normInv = 1 / ( std::sqrt( ROIcount * ROIvar ) );

  using MultiplyImageFilterType = itk::MultiplyImageFilter< FImageType3D, FImageType3D, FImageType3D >;
  MultiplyImageFilterType::Pointer multiplyImageFilter = MultiplyImageFilterType::New();
  multiplyImageFilter->SetInput( subtractConstantFromImageFilter->GetOutput() );
  multiplyImageFilter->SetConstant( normInv );

  FImageType3D::Pointer normalizedRoiImage = multiplyImageFilter->GetOutput();
  /////////////// End of normalization of roiImage //////////////

  if ( globalImagedebugLevel > 8 )
  {
    std::string normalized_roiImage_name( this->m_ResultsDir + "/NormalizedRoiImage_" +
                                          itksys::SystemTools::GetFilenameName( mapID ) + ".nii.gz" );
    itkUtil::WriteImage< FImageType3D >( normalizedRoiImage, normalized_roiImage_name );

    std::string roiMask_name( this->m_ResultsDir + "/roiMask_" + itksys::SystemTools::GetFilenameName( mapID ) +
                              ".nii.gz" );
    itkUtil::WriteImage< SImageType >( roiMask, roiMask_name );
  }

  // Now each landmark template should be converted to a moving template image
  //
  FImageType3D::Pointer lmkTemplateImage = FImageType3D::New();
  lmkTemplateImage->SetOrigin( roiImage->GetOrigin() );
  lmkTemplateImage->SetSpacing( roiImage->GetSpacing() );
  lmkTemplateImage->SetDirection( roiImage->GetDirection() );
  FImageType3D::SizeType mi_size;
  mi_size[0] = 2 * height + 1;
  mi_size[1] = 2 * radii + 1;
  mi_size[2] = 2 * radii + 1;
  FImageType3D::IndexType mi_start;
  mi_start.Fill( 0 );
  FImageType3D::RegionType mi_region( mi_start, mi_size );
  lmkTemplateImage->SetRegions( mi_region );
  lmkTemplateImage->Allocate();

  // Since each landmark template is a cylinder, a template mask is needed.
  //
  SImageType::Pointer templateMask = SImageType::New();
  templateMask->CopyInformation( lmkTemplateImage );
  templateMask->SetRegions( lmkTemplateImage->GetLargestPossibleRegion() );
  templateMask->Allocate();

  // Fill the template moving image based on the vector index locations
  //  and template mean values for different angle rotations
  //
  double cc_rotation_max = 0.0;
  for ( unsigned int curr_rotationAngle = 0; curr_rotationAngle < TemplateMean.size(); curr_rotationAngle++ )
  {
    lmkTemplateImage->FillBuffer( 0 );
    templateMask->FillBuffer( 0 );
    // iterate over mean values for the current rotation angle
    std::vector< float >::const_iterator mean_iter = TemplateMean[curr_rotationAngle].begin();
    // Fill the lmk template image using the mean values
    for ( landmarksConstellationModelIO::IndexLocationVectorType::const_iterator it = model.begin(); it != model.end();
          ++it, ++mean_iter )
    {
      FImageType3D::IndexType pixelIndex;
      pixelIndex[0] = ( *it )[0] + height;
      pixelIndex[1] = ( *it )[1] + radii;
      pixelIndex[2] = ( *it )[2] + radii;
      lmkTemplateImage->SetPixel( pixelIndex, *mean_iter );
      templateMask->SetPixel( pixelIndex, 1 );
    }
    if ( globalImagedebugLevel > 8 )
    {
      std::string tmpImageName( this->m_ResultsDir + "/lmkTemplateImage_" +
                                itksys::SystemTools::GetFilenameName( mapID ) + "_" +
                                local_to_string( curr_rotationAngle ) + ".nii.gz" );
      itkUtil::WriteImage< FImageType3D >( lmkTemplateImage, tmpImageName );

      std::string tmpMaskName( this->m_ResultsDir + "/templateMask_" + itksys::SystemTools::GetFilenameName( mapID ) +
                               "_" + local_to_string( curr_rotationAngle ) + ".nii.gz" );
      itkUtil::WriteImage< SImageType >( templateMask, tmpMaskName );
    }

    // Finally NCC is calculated in frequency domain
    //
    using CorrelationFilterType =
      itk::MaskedFFTNormalizedCorrelationImageFilter< FImageType3D, FImageType3D, SImageType >;
    CorrelationFilterType::Pointer correlationFilter = CorrelationFilterType::New();
    correlationFilter->SetFixedImage( normalizedRoiImage );
    correlationFilter->SetFixedImageMask( roiMask );
    correlationFilter->SetMovingImage( lmkTemplateImage );
    correlationFilter->SetMovingImageMask( templateMask );
    correlationFilter->SetRequiredFractionOfOverlappingPixels( 1 );
    correlationFilter->Update();
    if ( globalImagedebugLevel > 8 )
    {
      std::string ncc_output_name( this->m_ResultsDir + "/NCCOutput_" + itksys::SystemTools::GetFilenameName( mapID ) +
                                   "_" + local_to_string( curr_rotationAngle ) + ".nii.gz" );
      itkUtil::WriteImage< FImageType3D >( correlationFilter->GetOutput(), ncc_output_name );
    }

    // Maximum NCC for current rotation angle
    using MinimumMaximumImageCalculatorType = itk::MinimumMaximumImageCalculator< FImageType3D >;
    MinimumMaximumImageCalculatorType::Pointer minimumMaximumImageCalculatorFilter =
      MinimumMaximumImageCalculatorType::New();
    minimumMaximumImageCalculatorFilter->SetImage( correlationFilter->GetOutput() );
    minimumMaximumImageCalculatorFilter->Compute();
    double cc = minimumMaximumImageCalculatorFilter->GetMaximum();
    if ( cc > cc_rotation_max )
    {
      cc_rotation_max = cc;
      // Where maximum happens
      FImageType3D::IndexType maximumCorrelationPatchCenter = minimumMaximumImageCalculatorFilter->GetIndexOfMaximum();
      correlationFilter->GetOutput()->TransformIndexToPhysicalPoint( maximumCorrelationPatchCenter, GuessPoint );
    }
  }
  cc_Max = cc_rotation_max;

  if ( LMC::globalverboseFlag )
  {
    std::cout << "cc max: " << cc_Max << std::endl;
    std::cout << "guessed point in physical space: " << GuessPoint << std::endl;
  }
  return GuessPoint;
}

void
landmarksConstellationDetector::EulerToVersorRigid( VersorTransformType::Pointer &         result,
                                                    const RigidTransformType::ConstPointer eulerRigid )
{
  if ( result.IsNotNull() && eulerRigid.IsNotNull() )
  {
    result->SetFixedParameters( eulerRigid->GetFixedParameters() );
    itk::Versor< double >               versorRotation;
    const itk::Matrix< double, 3, 3 > & CleanedOrthogonalized =
      itk::Orthogonalize3DRotationMatrix( eulerRigid->GetMatrix() );
    versorRotation.Set( CleanedOrthogonalized );
    result->SetRotation( versorRotation );
    result->SetTranslation( eulerRigid->GetTranslation() );
  }
  else
  {
    itkGenericExceptionMacro( << "Error missing Pointer data, assigning "
                              << "Euler3DTransformPointer to VersorRigid3DTransformPointer." << std::endl );
  }
}

void
landmarksConstellationDetector::DoResampleInPlace( const SImageType::ConstPointer         inputImg,
                                                   const RigidTransformType::ConstPointer rigidTx,
                                                   SImageType::Pointer &                  inPlaceResampledImg )
{
  VersorTransformType::Pointer versorRigidTx = VersorTransformType::New();
  EulerToVersorRigid( versorRigidTx, rigidTx.GetPointer() );

  using ResampleIPFilterType = itk::ResampleInPlaceImageFilter< SImageType, SImageType >;
  using ResampleIPFilterPointer = ResampleIPFilterType::Pointer;
  ResampleIPFilterPointer InPlaceResampler = ResampleIPFilterType::New();
  InPlaceResampler->SetInputImage( inputImg );
  InPlaceResampler->SetRigidTransform( versorRigidTx.GetPointer() );
  InPlaceResampler->Update();

  inPlaceResampledImg = InPlaceResampler->GetOutput();
}

void
landmarksConstellationDetector::Compute( SImageType::Pointer orig_space_image )
{
  std::cout << "\nEstimating MSP..." << std::endl;

  // save the result that whether we are going to process all the landmarks
  // in light of user-specified eye center info.
  bool hasUserSpecEyeCenterInfo = true;
  bool hasUserForcedRPPoint = true;
  bool hasUserForcedACPoint = true;
  bool hasUserForcedPCPoint = true;
  bool hasUserForcedVN4Point = true;

  if ( ( this->m_msp_lmks.find( "LE" ) == this->m_msp_lmks.end() ) ||
       ( this->m_msp_lmks.find( "RE" ) == this->m_msp_lmks.end() ) )
  {
    hasUserSpecEyeCenterInfo = false;
  }

  if ( this->m_orig_lmks_constant.find( "RP" ) == this->m_orig_lmks_constant.end() )
  {
    hasUserForcedRPPoint = false;
  }

  if ( this->m_orig_lmks_constant.find( "AC" ) == this->m_orig_lmks_constant.end() )
  {
    hasUserForcedACPoint = false;
  }

  if ( this->m_orig_lmks_constant.find( "PC" ) == this->m_orig_lmks_constant.end() )
  {
    hasUserForcedPCPoint = false;
  }

  if ( this->m_orig_lmks_constant.find( "VN4" ) == this->m_orig_lmks_constant.end() )
  {
    hasUserForcedVN4Point = false;
  }

  if ( globalImagedebugLevel > 2 )
  {
    LandmarksMapType eyeFixed_lmks;
    eyeFixed_lmks["CM"] = this->m_eyeFixed_lmk_CenterOfHeadMass;
    const std::string roughlyAlignedCHMName( this->m_ResultsDir + "/eyeFixed_lmks.fcsv" );
    WriteITKtoSlicer3Lmk( roughlyAlignedCHMName, eyeFixed_lmks );

    const std::string roughlyAlignedVolumeName( this->m_ResultsDir + "/VolumeRoughAlignedWithHoughEye.nrrd" );
    itkUtil::WriteImage< SImageType >( this->m_eyeFixed_img, roughlyAlignedVolumeName );
  }

  // Compute the estimated MSP transform, and aligned image
  double c_c = 0;
  ComputeMSP( this->m_eyeFixed_img,
              this->m_test_orig2msp_img_tfm,
              this->m_msp_img,
              this->m_eyeFixed_lmk_CenterOfHeadMass,
              this->m_mspQualityLevel,
              c_c );
#if 0
  /*
   * If the MSP estimation is not good enough (i.e. c_c < -0.64), we try to compute the MSP again
   * using the previous m_test_orig2msp_img_tfm as an initial transform.
   * Threshold is set to 0.64 based on the statistical calculations on 23 successfully passed data.
   */
  if( c_c > -0.64 && !this->m_HoughEyeFailure )
    {
      unsigned int maxNumberOfIterations = 5;
      std::cout << "\n============================================================="
                << "\nBad Estimation for MSP Plane.\n"
                << "Repeat the Estimation Process up to " << maxNumberOfIterations
                << " More Times to Find a Better Estimation..." << std::endl;

      for (unsigned int i = 0; i<maxNumberOfIterations; i++)
        {
        std::cout << "\nTry " << i+1 << "..." << std::endl;

        // Rotate VolumeRoughAlignedWithHoughEye by finalTmsp again.
        SImageType::Pointer localRoughAlignedInput;
        DoResampleInPlace( this->m_eyeFixed_img.GetPointer(), // input
                           this->m_test_orig2msp_img_tfm.GetPointer(), // input
                           localRoughAlignedInput ); // output

        // Transform the eyeFixed_lmk_CenterOfHeadMass by finlaTmsp transform.
        VersorTransformType::Pointer localInvFinalTmsp = VersorTransformType::New();
        this->m_test_orig2msp_img_tfm->GetInverse( localInvFinalTmsp );
        SImageType::PointType localAlignedCOHM = localInvFinalTmsp->TransformPoint( this->m_eyeFixed_lmk_CenterOfHeadMass );

        if( globalImagedebugLevel > 2 )
          {
          LandmarksMapType locallyRotatedCHM;
          locallyRotatedCHM["CM"] = localAlignedCOHM;
          const std::string localRotatedCenterOfHeadMassNAME
                            ( this->m_ResultsDir + "/localRotatedCHM_" + local_to_string(i) + "_.fcsv" );
          WriteITKtoSlicer3Lmk( localRotatedCenterOfHeadMassNAME, locallyRotatedCHM );

          const std::string localRotatedRoughAligedVolumeNAME
                            ( this->m_ResultsDir + "/localRotatedRoughAlignedVolume_" + local_to_string(i) + "_.nrrd" );
          itkUtil::WriteImage<SImageType> ( localRoughAlignedInput, localRotatedRoughAligedVolumeNAME );
          }

        // Compute MSP again
        RigidTransformType::Pointer localTmsp;
        c_c = 0;
        ComputeMSP( localRoughAlignedInput, localTmsp,
                   this->m_msp_img, localAlignedCOHM, this->m_mspQualityLevel, c_c );

        // Compose the eyeFixed2msp_lmk_tfm transforms
        this->m_test_orig2msp_img_tfm->Compose(localTmsp);

        if ( c_c < -0.64 )
          {
          break;
          }
        }

      using StatisticsFilterType = itk::StatisticsImageFilter<SImageType>;
      StatisticsFilterType::Pointer statisticsFilter = StatisticsFilterType::New();
      statisticsFilter->SetInput(this->m_eyeFixed_img);
      statisticsFilter->Update();
      SImageType::PixelType minPixelValue = statisticsFilter->GetMinimum();

      this->m_msp_img = TransformResample<SImageType, SImageType>( this->m_eyeFixed_img.GetPointer(),
                                                                    MakeIsoTropicReferenceImage().GetPointer(),
                                                                    minPixelValue,
                                                                    GetInterpolatorFromString<SImageType>("Linear").GetPointer(),
                                                                    this->GetTransformToMSP().GetPointer() );
    }
#endif
  std::cout << "\n=============================================================" << std::endl;

  // Generate a warning if reflective correlation similarity measure is low.
  // It may be normal in some very diseased subjects, so don't throw an exception here.
  if ( c_c > -0.64 )
  {
    std::cout << "WARNING: Low reflective correlation between left/right hemispheres." << std::endl
              << "The estimated landmarks may not be reliable.\n"
              << std::endl;
  }

  // Throw an exception and stop BCD if RC metric is too low (less than 0.4) because results will not be reliable.
  if ( c_c > -0.40 )
  {
    itkGenericExceptionMacro( << "Too large MSP estimation error! reflective correlation metric is: " << c_c
                              << std::endl
                              << "Estimation of landmarks will not be reliable.\n"

                              << std::endl );
  }

  // In case hough eye detector failed
  if ( this->m_HoughEyeFailure || ( globalImagedebugLevel > 1 ) )
  {
    const std::string EMSP_Fiducial_file_name( "EMSP.fcsv" );
    std::stringstream failureMessageStream( "" );
    failureMessageStream << "EMSP aligned image and zero eye centers "
                         << "landmarks are written to " << std::endl
                         << this->m_ResultsDir << ". Use GUI corrector to "
                         << "correct the landmarks in." << EMSP_Fiducial_file_name
                         << "INITIAL LMKS: " << EMSP_Fiducial_file_name << "FOR IMAGE: " << this->m_msp_img
                         << "IN DIR: " << this->m_ResultsDir << std::endl;
    LandmarksMapType zeroEyeCenters;
    if ( this->m_HoughEyeFailure )
    {
      SImageType::PointType zeroPoint;
      zeroPoint.Fill( 0 );

      zeroEyeCenters["LE"] = zeroPoint;
      zeroEyeCenters["RE"] = zeroPoint;
    }
    WriteManualFixFiles( EMSP_Fiducial_file_name,
                         this->m_msp_img,
                         this->m_ResultsDir,
                         zeroEyeCenters,
                         failureMessageStream.str(),
                         this->m_HoughEyeFailure );
  }

  if ( globalImagedebugLevel > 2 )
  {
    const std::string MSP_ImagePlaneForOrig( this->m_ResultsDir + "/MSP_PLANE_For_Orig.nii.gz" );
    CreatedebugPlaneImage( this->m_eyeFixed_img, MSP_ImagePlaneForOrig );
    const std::string MSP_ImagePlane( this->m_ResultsDir + "/MSP_PLANE.nii.gz" );
    CreatedebugPlaneImage( this->m_msp_img, MSP_ImagePlane );
  }

  // HACK:  TODO:  DEBUG: Need to remove redundant need for volume and mask when
  // they can be the same image ( perhaps with a threshold );
  SImageType::Pointer mask_LR = this->m_msp_img;

  VersorTransformType::Pointer orig2eyeFixed_lmk_tfm;
  if ( !hasUserSpecEyeCenterInfo )
  {
    orig2eyeFixed_lmk_tfm = VersorTransformType::New();
    this->m_orig2eyeFixed_img_tfm->GetInverse( orig2eyeFixed_lmk_tfm );
  }

  VersorTransformType::Pointer eyeFixed2msp_lmk_tfm = VersorTransformType::New();
  this->m_test_orig2msp_img_tfm->GetInverse( eyeFixed2msp_lmk_tfm );

  this->m_msp_lmk_CenterOfHeadMass = eyeFixed2msp_lmk_tfm->TransformPoint( this->m_eyeFixed_lmk_CenterOfHeadMass );
  this->m_msp_lmk_CenterOfHeadMass[0] = 0; // Search starts on the estimated MSP

  {
    SImageType::PointType msp_lmk_RP_Candidate;
    SImageType::PointType msp_lmk_PC_Candiate;
    SImageType::PointType msp_lmk_VN4_Candidate;
    SImageType::PointType msp_lmk_AC_Candidate;

    SImageType::PointType::VectorType RPtoCEC;
    SImageType::PointType::VectorType RPtoVN4;
    SImageType::PointType::VectorType RPtoAC;
    SImageType::PointType::VectorType RPtoPC;

    SImageType::PointType mspSpaceCEC;

    if ( !hasUserSpecEyeCenterInfo )
    {
      m_msp_lmks["LE"] =
        eyeFixed2msp_lmk_tfm->TransformPoint( orig2eyeFixed_lmk_tfm->TransformPoint( this->m_orig_lmk_LE ) );
      m_msp_lmks["RE"] =
        eyeFixed2msp_lmk_tfm->TransformPoint( orig2eyeFixed_lmk_tfm->TransformPoint( this->m_orig_lmk_RE ) );
    }

    mspSpaceCEC.SetToMidPoint( this->m_msp_lmks["LE"], this->m_msp_lmks["RE"] );
    mspSpaceCEC[0] = 0; // Search starts on the estimated MSP

    std::cout << "\nPerforming morphmetric search + local search..." << std::endl;
    {
      /*
       * Search for MPJ ( RP )
       *
       * Use the knowledge of mean CMtoRP vector + local searching
       */
      std::cout << "Processing MPJ..." << std::endl;
      double searchRadiusLR = 4.; // in mm, and it is only for landmarks near
                                  // MSP
      double cc_RP_Max = 0;
      if ( this->m_msp_lmks.find( "RP" ) != this->m_msp_lmks.end() )
      {
        msp_lmk_RP_Candidate = this->m_msp_lmks["RP"];
        std::cout << "Skip estimation, directly load from file." << std::endl;
      }
      else if ( hasUserForcedRPPoint )
      {
        std::cout << "Skip estimation, directly forced by command line." << std::endl;
        msp_lmk_RP_Candidate = eyeFixed2msp_lmk_tfm->TransformPoint(
          orig2eyeFixed_lmk_tfm->TransformPoint( this->m_orig_lmks_constant.at( "RP" ) ) );
      }
      else
      {
        // The search radius of RP is set to 5 times larger than its template
        // radius in SI direction.
        // It's large because some scans have extra contents of neck or shoulder
        msp_lmk_RP_Candidate = FindCandidatePoints( this->m_msp_img,
                                                    mask_LR,
                                                    searchRadiusLR,
                                                    3. * this->m_TemplateRadius["RP"],
                                                    5. * this->m_TemplateRadius["RP"],
                                                    this->m_msp_lmk_CenterOfHeadMass.GetVectorFromOrigin() +
                                                      this->m_InputTemplateModel.GetCMtoRPMean(),
                                                    this->m_InputTemplateModel.GetTemplateMeans( "RP" ),
                                                    this->m_InputTemplateModel.m_VectorIndexLocations["RP"],
                                                    cc_RP_Max,
                                                    "RP" );
      }

      // Local search radius in LR direction is affected by the
      // estimated MSP error in LR direction
      const double err_MSP = std::abs( msp_lmk_RP_Candidate[0] - this->m_msp_lmk_CenterOfHeadMass[0] );
      std::cout << "The estimated MSP error in LR direction: " << err_MSP << " mm" << std::endl;

      if ( err_MSP < 1 )
      {
        searchRadiusLR = 1.;
        std::cout << "Local search radius in LR direction is set to 1 mm." << std::endl;
      }
      else if ( err_MSP < 2 )
      {
        searchRadiusLR = 2.;
        std::cout << "Local search radius in LR direction is set to 2 mm." << std::endl;
      }
      else if ( err_MSP > 6 )
      {
        itkGenericExceptionMacro( << "Bad MPJ estimation or too large MSP estimation error!"
                                  << "The estimation result is probably not reliable." );
      }
      else // not changed
      {
        std::cout << "Local search radius in LR direction is set to 4 mm." << std::endl;
      }

      /*
       * Search for 4VN
       *
       * we will use RPtoCEC ( RP-to-center of eye centers mean vector ) and RPtoVN4Mean
       * to determine the search center for VN4
       *
       * Assumption:
       * 1. Human brains share a similar angle from RPtoCEC to RPtoVN4
       * 2. Human brains share a similar RPtoVN4 norm
       * 3. The center of eye centers, MPJ, and AC are all very close to the MSP plane
       * 4. VN4 is always below CEC-MPJ line on MSP plane
       */
      std::cout << "Processing 4VN..." << std::endl;
      RPtoCEC = mspSpaceCEC - msp_lmk_RP_Candidate;

      // RPtoVN4 = this->m_InputTemplateModel.GetRPtoXMean( "VN4" );
      RPtoVN4 = FindVectorFromPointAndVectors(
        RPtoCEC, this->m_InputTemplateModel.GetRPtoCECMean(), this->m_InputTemplateModel.GetRPtoXMean( "VN4" ), -1 );

      double cc_VN4_Max = 0;

      if ( this->m_msp_lmks.find( "VN4" ) != this->m_msp_lmks.end() )
      {
        msp_lmk_VN4_Candidate = this->m_msp_lmks["VN4"];
        std::cout << "Skip estimation, directly load from file." << std::endl;
      }
      else if ( hasUserForcedVN4Point )
      {
        std::cout << "Skip estimation, directly forced by command line." << std::endl;
        msp_lmk_VN4_Candidate = eyeFixed2msp_lmk_tfm->TransformPoint(
          orig2eyeFixed_lmk_tfm->TransformPoint( this->m_orig_lmks_constant.at( "VN4" ) ) );
      }
      else
      {
        msp_lmk_VN4_Candidate = FindCandidatePoints( this->m_msp_img,
                                                     mask_LR,
                                                     searchRadiusLR,
                                                     1.6 * this->m_TemplateRadius["VN4"],
                                                     1.6 * this->m_TemplateRadius["VN4"],
                                                     msp_lmk_RP_Candidate.GetVectorFromOrigin() + RPtoVN4,
                                                     this->m_InputTemplateModel.GetTemplateMeans( "VN4" ),
                                                     this->m_InputTemplateModel.m_VectorIndexLocations["VN4"],
                                                     cc_VN4_Max,
                                                     "VN4" );
      }

      /*
       * Search for AC
       *
       * Assumption:
       * 1. Human brains share a similar angle from RPtoCEC to RPtoAC
       * 2. Human brains share a similar RPtoAC norm
       * 3. The center of eye centers, MPJ, and AC are all very close to the MSP plane
       * 4. AC is always above CEC-MPJ line on MSP plane
       */
      std::cout << "Processing AC..." << std::endl;

      // RPtoAC = this->m_InputTemplateModel.GetRPtoXMean( "AC" );
      RPtoAC = FindVectorFromPointAndVectors(
        RPtoCEC, this->m_InputTemplateModel.GetRPtoCECMean(), this->m_InputTemplateModel.GetRPtoXMean( "AC" ), 1 );
      double cc_AC_Max = 0;
      if ( this->m_msp_lmks.find( "AC" ) != this->m_msp_lmks.end() )
      {
        msp_lmk_AC_Candidate = this->m_msp_lmks["AC"];
        std::cout << "Skip estimation, directly load from file." << std::endl;
      }
      else if ( hasUserForcedACPoint )
      {
        std::cout << "Skip estimation, directly forced by command line." << std::endl;
        msp_lmk_AC_Candidate = eyeFixed2msp_lmk_tfm->TransformPoint(
          orig2eyeFixed_lmk_tfm->TransformPoint( this->m_orig_lmks_constant.at( "AC" ) ) );
      }
      else
      {
        msp_lmk_AC_Candidate = FindCandidatePoints( this->m_msp_img,
                                                    mask_LR,
                                                    searchRadiusLR,
                                                    1.6 * this->m_TemplateRadius["AC"],
                                                    1.6 * this->m_TemplateRadius["AC"],
                                                    msp_lmk_RP_Candidate.GetVectorFromOrigin() + RPtoAC,
                                                    this->m_InputTemplateModel.GetTemplateMeans( "AC" ),
                                                    this->m_InputTemplateModel.m_VectorIndexLocations["AC"],
                                                    cc_AC_Max,
                                                    "AC" );
      }

      /*
       * Search for PC
       *
       * Assumption:
       * 1. Human brains share a similar angle from RPtoCEC to RPtoPC
       * 2. Human brains share a similar RPtoPC norm
       * 3. The center of eye centers, MPJ, and PC are all very close to the MSP plane
       * 4. PC is always above CEC-MPJ line on MSP plane
       */
      std::cout << "Processing PC..." << std::endl;

      // RPtoPC = this->m_InputTemplateModel.GetRPtoXMean( "PC" );
      RPtoPC = FindVectorFromPointAndVectors(
        RPtoCEC, this->m_InputTemplateModel.GetRPtoCECMean(), this->m_InputTemplateModel.GetRPtoXMean( "PC" ), 1 );
      double cc_PC_Max = 0;
      if ( this->m_msp_lmks.find( "PC" ) != this->m_msp_lmks.end() )
      {
        msp_lmk_PC_Candiate = this->m_msp_lmks["PC"];
        std::cout << "Skip estimation, directly load from file." << std::endl;
      }
      else if ( hasUserForcedPCPoint )
      {
        std::cout << "Skip estimation, directly forced by command line." << std::endl;
        msp_lmk_PC_Candiate = eyeFixed2msp_lmk_tfm->TransformPoint(
          orig2eyeFixed_lmk_tfm->TransformPoint( this->m_orig_lmks_constant.at( "PC" ) ) );
      }
      else
      {
        msp_lmk_PC_Candiate = FindCandidatePoints( this->m_msp_img,
                                                   mask_LR,
                                                   searchRadiusLR,
                                                   4 * this->m_TemplateRadius["PC"],
                                                   4 * this->m_TemplateRadius["PC"],
                                                   msp_lmk_RP_Candidate.GetVectorFromOrigin() + RPtoPC,
                                                   this->m_InputTemplateModel.GetTemplateMeans( "PC" ),
                                                   this->m_InputTemplateModel.m_VectorIndexLocations["PC"],
                                                   cc_PC_Max,
                                                   "PC" );
      }

      // A check point for base landmarks
      if ( LMC::globalverboseFlag )
      {
        std::cout << cc_RP_Max << " " << cc_PC_Max << " " << cc_VN4_Max << " " << cc_AC_Max << "\n"
                  << msp_lmk_RP_Candidate << " " << msp_lmk_PC_Candiate << " " << msp_lmk_VN4_Candidate << " "
                  << msp_lmk_AC_Candidate << std::endl;
      }

      if ( globalImagedebugLevel > 1 )
      {
        std::string BrandedImageAName( this->m_ResultsDir + "/BrandedImage.png" );

        MakeBrandeddebugImage( this->m_msp_img.GetPointer(),
                               this->m_InputTemplateModel,
                               this->m_msp_lmk_CenterOfHeadMass + this->m_InputTemplateModel.GetCMtoRPMean(),
                               msp_lmk_RP_Candidate + RPtoAC,
                               msp_lmk_RP_Candidate + RPtoPC,
                               msp_lmk_RP_Candidate + RPtoVN4,
                               BrandedImageAName,
                               msp_lmk_RP_Candidate,
                               msp_lmk_AC_Candidate,
                               msp_lmk_PC_Candiate,
                               msp_lmk_VN4_Candidate );

        if ( globalImagedebugLevel > 3 )
        {
          std::string LabelImageAName( this->m_ResultsDir + "/MSP_Mask.nii.gz" );
          MakeLabelImage( this->m_msp_img,
                          msp_lmk_RP_Candidate,
                          msp_lmk_AC_Candidate,
                          msp_lmk_PC_Candiate,
                          msp_lmk_VN4_Candidate,
                          LabelImageAName );
        }
      }

      // ============================================================================================
      //  Update landmark points


      // After finding RP, AC, PC, we can compute the versor transform for
      // registration
      // Also in this stage, we store some results for later use
      // Save named points in original space
      if ( !hasUserForcedRPPoint )
      {
        this->m_orig_lmks_updated["RP"] = this->m_test_orig2msp_img_tfm->TransformPoint( msp_lmk_RP_Candidate );
      }

      if ( !hasUserForcedVN4Point )
      {
        this->m_orig_lmks_updated["VN4"] = this->m_test_orig2msp_img_tfm->TransformPoint( msp_lmk_VN4_Candidate );
      }

      if ( !hasUserForcedACPoint )
      {
        this->m_orig_lmks_updated["AC"] = this->m_test_orig2msp_img_tfm->TransformPoint( msp_lmk_AC_Candidate );
      }

      if ( !hasUserForcedPCPoint )
      {
        this->m_orig_lmks_updated["PC"] = this->m_test_orig2msp_img_tfm->TransformPoint( msp_lmk_PC_Candiate );
      }

      this->m_orig_lmks_updated["CM"] =
        this->m_test_orig2msp_img_tfm->TransformPoint( this->m_msp_lmk_CenterOfHeadMass );

      if ( !hasUserSpecEyeCenterInfo )
      {
        if ( !hasUserForcedRPPoint )
        {
          m_orig_lmks_updated["RP"] =
            this->m_orig2eyeFixed_img_tfm->TransformPoint( this->m_orig_lmks_constant.at( "RP" ) );
        }
        if ( !hasUserForcedVN4Point )
        {
          m_orig_lmks_updated["VN4"] =
            this->m_orig2eyeFixed_img_tfm->TransformPoint( this->m_orig_lmks_constant.at( "VN4" ) );
        }
        if ( !hasUserForcedACPoint )
        {
          m_orig_lmks_updated["AC"] =
            this->m_orig2eyeFixed_img_tfm->TransformPoint( this->m_orig_lmks_constant.at( "AC" ) );
        }
        if ( !hasUserForcedPCPoint )
        {
          m_orig_lmks_updated["PC"] =
            this->m_orig2eyeFixed_img_tfm->TransformPoint( this->m_orig_lmks_constant.at( "PC" ) );
        }
        m_orig_lmks_updated["CM"] =
          this->m_orig2eyeFixed_img_tfm->TransformPoint( this->m_orig_lmks_constant.at( "CM" ) );
        m_orig_lmks_updated["LE"] = this->m_orig_lmk_LE;
        m_orig_lmks_updated["RE"] = this->m_orig_lmk_RE;
      }
      else
      {
        this->m_orig_lmks_updated["LE"] = this->m_test_orig2msp_img_tfm->TransformPoint( this->m_msp_lmks.at( "LE" ) );
        this->m_orig_lmks_updated["RE"] = this->m_test_orig2msp_img_tfm->TransformPoint( this->m_msp_lmks.at( "RE" ) );
      }

      // Write some debug images
      {
        if ( globalImagedebugLevel > 3 )
        {
          std::string ResampledMaskmageName( this->m_ResultsDir + "/Resampled_Mask.nii.gz" );
          MakeLabelImage( this->m_eyeFixed_img,
                          msp_lmk_RP_Candidate,
                          msp_lmk_AC_Candidate,
                          msp_lmk_PC_Candiate,
                          msp_lmk_VN4_Candidate,
                          ResampledMaskmageName );

          std::string OrigMaskImageName( this->m_ResultsDir + "/Orig_Mask.nii.gz" );
          MakeLabelImage( this->m_eyeFixed_img,
                          this->m_orig_lmks_constant.at( "RP" ),
                          this->m_orig_lmks_constant.at( "AC" ),
                          this->m_orig_lmks_constant.at( "PC" ),
                          this->m_orig_lmks_constant.at( "VN4" ),
                          OrigMaskImageName );
        }
      }

      // Save some named points in EMSP space mainly for debug use
      {
        this->m_msp_lmks["AC"] = msp_lmk_AC_Candidate;
        this->m_msp_lmks["PC"] = msp_lmk_PC_Candiate;
        this->m_msp_lmks["RP"] = msp_lmk_RP_Candidate;
        this->m_msp_lmks["VN4"] = msp_lmk_VN4_Candidate;
        this->m_msp_lmks["CM"] = this->m_msp_lmk_CenterOfHeadMass;
        // Eye centers in EMSP have been saved in a earlier time
      }

      // Compute the AC-PC aligned transform
      // Note for the sake of EPCA, we need the transform at this stage
      // so that the method is robust against rotation
      this->m_orig2msp_img_tfm = this->Compute_orig2msp_img_tfm( m_orig_lmks_updated );
      // NOTE: LandmarkTransforms are inverse of ImageTransforms, (You pull images, you push landmarks)
      VersorTransformType::Pointer orig2msp_lmk_tfm =
        GetLandmarkTransformFromImageTransform( this->m_orig2msp_img_tfm.GetPointer() );

      // Save some named points in AC-PC aligned space
      LandmarksMapType msp_lmks; // named points in ACPC-aligned space
      {
        msp_lmks["AC"] = orig2msp_lmk_tfm->TransformPoint( this->m_orig_lmks_constant.at( "AC" ) );
        msp_lmks["PC"] = orig2msp_lmk_tfm->TransformPoint( this->m_orig_lmks_constant.at( "PC" ) );
        msp_lmks["RP"] = orig2msp_lmk_tfm->TransformPoint( this->m_orig_lmks_constant.at( "RP" ) );
        msp_lmks["VN4"] = orig2msp_lmk_tfm->TransformPoint( this->m_orig_lmks_constant.at( "VN4" ) );
        msp_lmks["CM"] = orig2msp_lmk_tfm->TransformPoint( this->m_orig_lmks_constant.at( "CM" ) );
        msp_lmks["LE"] = orig2msp_lmk_tfm->TransformPoint( this->m_orig_lmks_constant.at( "LE" ) );
        msp_lmks["RE"] = orig2msp_lmk_tfm->TransformPoint( this->m_orig_lmks_constant.at( "RE" ) );
      }

      // Get a copy of landmarks on ACPC plane for eliminating accumulative
      // errors of local search process
      LandmarksMapType raw_msp_lmks = msp_lmks; // reserve this map to avoid the accumulation of local search errors


      /*
       * For the rest of the landmarks
       *
       * The search center of the rest of the landmarks will be estimated
       * by linear model estimation with EPCA
       * Note The current version use landmarks in acpc-aligned space.
       */
      {
        // Build up an evolutionary processing list
        // order: RP, AC, PC, VN4, LE, RE, ...
        // Note this order should comply with the order we defined in LLS model
        std::vector< std::string > processingList{ "RP", "AC", "PC", "VN4", "LE", "RE" };
        unsigned int               numBaseLandmarks = 6;
        unsigned int               dim = 3;
        for ( unsigned int ii = 1; ii <= m_LlsMatrices.size(); ++ii )
        {
          // The processing order is indicated by the length of EPCA coefficient
          auto iit = this->m_LlsMatrices.begin();
          while ( iit->second.columns() != ( numBaseLandmarks + ii - 2 ) * dim )
          {
            if ( iit != this->m_LlsMatrices.end() )
            {
              ++iit; // transversal
            }
            else
            {
              std::cerr << "Error: wrong number of parameters for linear model!" << std::endl;
              std::cerr << "Some EPCA landmarks may not be detected." << std::endl;
              return;
            }
          }

          std::cout << "Processing " << iit->first << "..." << std::endl;
          if ( this->m_msp_lmks.find( iit->first ) != this->m_msp_lmks.end() )
          {
            std::cout << "Skip estimation, directly load from file." << std::endl;

            // HACK : The following looks like an identity mapping
            msp_lmks[iit->first] = orig2msp_lmk_tfm->TransformPoint(
              this->m_test_orig2msp_img_tfm->TransformPoint( this->m_msp_lmks.at( iit->first ) ) );
            raw_msp_lmks[iit->first] = msp_lmks.at( iit->first );
          }
          else
          {
            // in every iteration, the last name in the processing list
            // indicates the landmark to be estimated
            processingList.push_back( iit->first );

            // Find search center by linear model estimation with
            // dimension reduction.
            // The result will be stored into this->m_msp_lmks[iit->first]
            LinearEstimation( raw_msp_lmks, processingList, numBaseLandmarks );

            // check whether it is midline landmark, set search range
            // and modify search center accordingly
            double localSearchRadiusLR = this->m_SearchRadii[iit->first];
            {
              std::vector< std::string >::const_iterator midlineIt = this->m_MidlinePointsList.begin();
              for ( ; midlineIt != this->m_MidlinePointsList.end(); ++midlineIt )
              {
                if ( ( *midlineIt ).compare( iit->first ) == 0 )
                {
                  localSearchRadiusLR = searchRadiusLR; // a variable
                                                        // changed with
                                                        // err_MSP
                  raw_msp_lmks[iit->first][0] = 0.;     // search
                                                        // starts
                                                        // near EMSP
                  break;
                }
              }
            }

            msp_lmks[iit->first][0] = raw_msp_lmks[iit->first][0];
            msp_lmks[iit->first][1] = raw_msp_lmks[iit->first][1];
            msp_lmks[iit->first][2] = raw_msp_lmks[iit->first][2];

            // Obtain the position of the current landmark in other spaces
            this->m_orig_lmks_updated[iit->first] =
              this->m_orig2msp_img_tfm->TransformPoint( msp_lmks.at( iit->first ) );
            if ( !hasUserSpecEyeCenterInfo )
            {
              this->m_msp_lmks[iit->first] =
                orig2eyeFixed_lmk_tfm->TransformPoint( this->m_orig_lmks_constant.at( iit->first ) );
              this->m_msp_lmks[iit->first] = eyeFixed2msp_lmk_tfm->TransformPoint( this->m_msp_lmks.at( iit->first ) );
            }
            else
            {
              this->m_msp_lmks[iit->first] =
                eyeFixed2msp_lmk_tfm->TransformPoint( this->m_orig_lmks_constant.at( iit->first ) );
            }

            // Enable local search
            if ( 1 )
            {
              // local search
              double cc_Max = 0;
              this->m_msp_lmks[iit->first] =
                FindCandidatePoints( this->m_msp_img,
                                     mask_LR,
                                     localSearchRadiusLR,
                                     this->m_SearchRadii[iit->first],
                                     this->m_SearchRadii[iit->first],
                                     this->m_msp_lmks[iit->first].GetVectorFromOrigin(),
                                     this->m_InputTemplateModel.GetTemplateMeans( iit->first ),
                                     this->m_InputTemplateModel.m_VectorIndexLocations[iit->first],
                                     cc_Max,
                                     iit->first );

              // Update landmarks in input and ACPC-aligned space
              this->m_orig_lmks_updated[iit->first] =
                this->m_test_orig2msp_img_tfm->TransformPoint( this->m_msp_lmks.at( iit->first ) );
              if ( !hasUserSpecEyeCenterInfo )
              {
                this->m_orig_lmks_updated[iit->first] =
                  this->m_orig2eyeFixed_img_tfm->TransformPoint( this->m_orig_lmks_constant.at( iit->first ) );
              }
              msp_lmks[iit->first] = orig2msp_lmk_tfm->TransformPoint( this->m_orig_lmks_constant.at( iit->first ) );
            }
          }
        } // End of arbitrary landmarks detection for the rest of "new" ones
      }   // End of arbitrary landmarks detection by linear model estimation

      this->ComputeFinalRefinedACPCAlignedTransform( orig_space_image );
      if ( globalImagedebugLevel > 1 ) // This may be very useful for GUI
                                       // corrector
      {
        WriteITKtoSlicer3Lmk( this->m_ResultsDir + "/EMSP.fcsv", this->m_msp_lmks );
      }
    } // End of local searching kernel
  }   // End of local searching
}


void
landmarksConstellationDetector::LinearEstimation( LandmarksMapType &                 namedPoints,
                                                  const std::vector< std::string > & processingList,
                                                  unsigned                           numBasePoints )
{
  unsigned int dim = namedPoints[processingList[0]].GetPointDimension();

  if ( processingList.size() <= numBasePoints )
  {
    std::cout << "No EPCA landmarks to be estimated." << std::endl;
    return;
  }
  std::string           newPointName = processingList[processingList.size() - 1];
  SImageType::PointType newPoint;
  newPoint.Fill( 0 );

  // Construct Xi_t
  VectorType Xi_t;
  Xi_t.set_size( dim * ( processingList.size() - 2 ) );
  for ( unsigned int k = 1; k <= processingList.size() - 2; ++k )
  {
    for ( unsigned int d = 0; d < dim; ++d )
    {
      Xi_t( ( k - 1 ) * dim + d ) =
        namedPoints[processingList[k]][d] - namedPoints[processingList[0]][d] - this->m_LlsMeans[newPointName][d];
    }
  }
  SImageType::PointType::VectorType tmp;
  tmp.SetVnlVector( this->m_LlsMatrices[newPointName] * Xi_t );
  newPoint = namedPoints[processingList[0]] + tmp;
  namedPoints[newPointName] = newPoint;

  // debug
  // std::cout << "Mi' = " << this->m_LlsMatrices[newPointName] << std::endl;
  // std::cout << "Xi_t = " << Xi_t << std::endl;
  // std::cout << "MPJ = " << namedPoints[processingList[0]] << std::endl;
  // std::cout << newPointName << " = " << newPoint << std::endl;
}

SImageType::PointType::VectorType
landmarksConstellationDetector::FindVectorFromPointAndVectors( SImageType::PointType::VectorType BA,
                                                               SImageType::PointType::VectorType BAMean,
                                                               SImageType::PointType::VectorType BCMean, int sign )
{
  SImageType::PointType::VectorType BC;

  double cosTheta; // cosine of the angle from BA to BC

  cosTheta = BAMean * BCMean / BAMean.GetNorm() / BCMean.GetNorm();

  // Start searching on MSP
  BC[0] = 0;
  double a = BA * BA;
  double b = -2. * BA.GetNorm() * BCMean.GetNorm() * BA[2] * cosTheta;
  double c = BCMean * BCMean * ( BA * BA * cosTheta * cosTheta - BA[1] * BA[1] );
  double delta = b * b - 4 * a * c;
  if ( delta < 0 )
  {
    itkGenericExceptionMacro( << "Failed to solve a 2rd-order equation!" );
  }
  else if ( sign == 1 || sign == -1 )
  {
    BC[2] = -( b - sign * sqrt( delta ) ) / 2. / a;
  }
  else
  {
    itkGenericExceptionMacro( << "Bad parameter! sign = 1 or sign = -1 please" );
  }
  BC[1] = ( BA.GetNorm() * BCMean.GetNorm() * cosTheta - BA[2] * BC[2] ) / BA[1];
  return BC;
}


void
WriteManualFixFiles( const std::string & EMSP_Fiducial_file_name, SImageType * const mspVolume,
                     const std::string & resultDir, const LandmarksMapType & errorLmks,
                     const std::string & failureMessage, const bool throwException )
{ // ADD MetaData for EMSP_FCSV_FILENAME
  itk::MetaDataDictionary & dict = mspVolume->GetMetaDataDictionary();
  const char * const        metaDataEMSP_FCSVName = "EMSP_FCSV_FILENAME";
  itk::EncapsulateMetaData< std::string >( dict, metaDataEMSP_FCSVName, EMSP_Fiducial_file_name.c_str() );

  // write EMSP aligned image
  itkUtil::WriteImage< SImageType >( mspVolume, resultDir + "/EMSP.nrrd" );

  if ( errorLmks.size() > 0 )
  {
    WriteITKtoSlicer3Lmk( resultDir + "/" + EMSP_Fiducial_file_name, errorLmks );
  }
  if ( throwException )
  {
    itkGenericExceptionMacro( << failureMessage );
  }
}
