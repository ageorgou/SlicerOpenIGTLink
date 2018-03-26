#include "vtkIGTLStreamingVolumeCodec.h"

// MRML includes
#include "vtkMRMLScene.h"
#include "vtkMRMLNRRDStorageNode.h"

// VTK includes
#include <vtkCollection.h>
#include <vtkMatrix4x4.h>
#include <vtkNew.h>
#include <vtkObjectFactory.h>
#include <vtkXMLUtilities.h>

// OpenIGTLinkIO includes
#include "igtlioVideoDevice.h"
#include "igtlioImageDevice.h"

//---------------------------------------------------------------------------
vtkMRMLCodecNewMacro(vtkIGTLStreamingVolumeCodec);
//---------------------------------------------------------------------------
vtkIGTLStreamingVolumeCodec::vtkIGTLStreamingVolumeCodec()
{
  this->DefaultVideoDevice = igtlio::VideoDevicePointer::New();
  this->LinkedDevice = NULL;
  this->CompressImageDeviceContent = false;
  this->Content.image = NULL;
  this->Content.frame = NULL;
  this->Content.keyFrame = NULL;
  
}

//---------------------------------------------------------------------------
vtkIGTLStreamingVolumeCodec::~vtkIGTLStreamingVolumeCodec()
{
}
//---------------------------------------------------------------------------
std::string vtkIGTLStreamingVolumeCodec::GetDeviceType() const
{
  return "igtlioVideoDevice";
}


//----------------------------------------------------------------------------
void vtkIGTLStreamingVolumeCodec::ProcessLinkedDeviceModifiedEvents( vtkObject *caller, unsigned long event, void *callData )
{
  igtlio::DevicePointer modifiedDevice = reinterpret_cast<igtlio::Device*>(caller);
  if (modifiedDevice->GetDeviceType().compare(igtlio::VideoConverter::GetIGTLTypeName()) == 0)
   {
    igtlio::VideoDevicePointer videoDevice = reinterpret_cast<igtlio::VideoDevice*>(caller);
    this->CopyVideoMessageIntoFrameMSG(videoDevice->GetCompressedIGTLMessage());
    this->CopyVideoMessageIntoKeyFrameMSG(videoDevice->GetKeyFrameMessage());
    Content.keyFrameUpdated = videoDevice->GetContent().keyFrameUpdated;
    }
  else if(modifiedDevice->GetDeviceType().compare(igtlio::ImageConverter::GetIGTLTypeName()) == 0 )
    {
    if (CompressImageDeviceContent)
      {
      igtlio::ImageDevicePointer imageDevice = reinterpret_cast<igtlio::ImageDevice*>(caller);
      this->Content.codecName = std::string(imageDevice->GetDeviceName());
      this->Content.image = imageDevice->GetContent().image;
      this->GetStreamFromContentUsingDefaultDevice(); // inside this line FrameMSG is copied.
      this->CopyVideoMessageIntoKeyFrameMSG(DefaultVideoDevice->GetKeyFrameMessage());
      Content.keyFrameUpdated = DefaultVideoDevice->GetContent().keyFrameUpdated;
      }
    }
    this->InvokeEvent(CodecModifiedEvent);
}

void vtkIGTLStreamingVolumeCodec::CopyVideoMessageIntoKeyFrameMSG(igtl::VideoMessage::Pointer keyFrameMsg)
{
  if (this->Content.keyFrame == NULL)
    this->Content.keyFrame = vtkSmartPointer<vtkUnsignedCharArray>::New();
  igtl_header* h_key = (igtl_header*) keyFrameMsg->GetPackPointer();
  igtl_header_convert_byte_order(h_key);
  Content.keyFrame->SetNumberOfTuples(keyFrameMsg->GetPackSize());
  char * keyFramePointer = reinterpret_cast<char*>(Content.keyFrame->GetPointer(0));
  memcpy(keyFramePointer, (char*)h_key, IGTL_HEADER_SIZE);
  memcpy(keyFramePointer+IGTL_HEADER_SIZE, keyFrameMsg->GetPackBodyPointer(), keyFrameMsg->GetPackSize() - IGTL_HEADER_SIZE);
}

void vtkIGTLStreamingVolumeCodec::CopyVideoMessageIntoFrameMSG(igtl::VideoMessage::Pointer frameMsg)
{
  if (this->Content.frame == NULL)
    this->Content.frame = vtkSmartPointer<vtkUnsignedCharArray>::New();
  igtl_header* h_key = (igtl_header*) frameMsg->GetPackPointer();
  igtl_header_convert_byte_order(h_key);
  this->Content.frame->SetNumberOfTuples(frameMsg->GetPackSize());
  char * framePointer = reinterpret_cast<char*>(Content.frame->GetPointer(0));
  memcpy(framePointer, (char*)h_key, IGTL_HEADER_SIZE);
  memcpy(framePointer+IGTL_HEADER_SIZE, frameMsg->GetPackBodyPointer(), frameMsg->GetPackSize() - IGTL_HEADER_SIZE);
}

int vtkIGTLStreamingVolumeCodec::LinkIGTLIOVideoDevice(igtlio::DevicePointer device)
{
  igtlio::VideoDevicePointer modifiedDevice = reinterpret_cast<igtlio::VideoDevice*>(device.GetPointer());
  this->Content.codecType= std::string(modifiedDevice->GetContent().videoMessage->GetCodecType());
  this->Content.codecName = std::string(modifiedDevice->GetDeviceName());
  this->Content.keyFrameUpdated = modifiedDevice->GetContent().keyFrameUpdated;
  this->Content.image = modifiedDevice->GetContent().image;
  this->CopyVideoMessageIntoFrameMSG(modifiedDevice->GetCompressedIGTLMessage());
  this->CopyVideoMessageIntoKeyFrameMSG(modifiedDevice->GetKeyFrameMessage());
  Content.keyFrameUpdated = modifiedDevice->GetContent().keyFrameUpdated;
  modifiedDevice->AddObserver(modifiedDevice->GetDeviceContentModifiedEvent(), this, &vtkIGTLStreamingVolumeCodec::ProcessLinkedDeviceModifiedEvents);
  this->LinkedDevice = device;
  return 0;
}

int vtkIGTLStreamingVolumeCodec::LinkIGTLIOImageDevice(igtlio::DevicePointer device)
{
  igtlio::ImageDevice* modifiedDevice = reinterpret_cast<igtlio::ImageDevice*>(device.GetPointer());
  
  this->Content.codecType = DefaultVideoDevice->GetContent().codecName;
  this->Content.codecName = std::string(modifiedDevice->GetDeviceName());
  igtlio::VideoConverter::ContentData deviceContent = DefaultVideoDevice->GetContent();
  deviceContent.image = modifiedDevice->GetContent().image;
  DefaultVideoDevice->SetContent(deviceContent);
  this->Content.image = modifiedDevice->GetContent().image;
  this->GetStreamFromContentUsingDefaultDevice(); // in this line FrameMSG is updated.
  modifiedDevice->AddObserver(modifiedDevice->GetDeviceContentModifiedEvent(), this, &vtkIGTLStreamingVolumeCodec::ProcessLinkedDeviceModifiedEvents);
  this->LinkedDevice = device;
  return 0;
}

//---------------------------------------------------------------------------
int vtkIGTLStreamingVolumeCodec::UncompressedDataFromStream(vtkSmartPointer<vtkUnsignedCharArray> bitStreamData, bool checkCRC)
{
  //To do : use the buffer to update Content.image
   if (this->LinkedDevice == NULL && this->DefaultVideoDevice == NULL)
    {
      vtkWarningMacro("Video Devices are NULL, message not generated.")
      return 0;
    }
  if (bitStreamData == NULL)
    {
    return 0;
    }
  if (bitStreamData->GetNumberOfValues() == 0)
    {
    vtkWarningMacro("message size equal to zero.")
    return 0;
    }
  igtl::MessageHeader::Pointer headerMsg = igtl::MessageHeader::New();
  headerMsg->InitPack();
  memcpy(headerMsg->GetPackPointer(), bitStreamData->GetPointer(0), headerMsg->GetPackSize());
  headerMsg->Unpack();
  igtl::MessageBase::Pointer buffer = igtl::MessageBase::New();
  buffer->SetMessageHeader(headerMsg);
  buffer->AllocatePack();
  memcpy(buffer->GetPackBodyPointer(), bitStreamData->GetPointer(0)+IGTL_HEADER_SIZE, buffer->GetPackBodySize());
  if (strcmp(headerMsg->GetDeviceType(), igtlio::ImageConverter::GetIGTLTypeName()) == 0 ||
      this->LinkedDevice == NULL ||
      this->LinkedDevice->GetDeviceType().compare(igtlio::ImageConverter::GetIGTLTypeName()) == 0)
    {
    this->DefaultVideoDevice->SetDeviceName(buffer->GetDeviceName());
    if (this->DefaultVideoDevice->ReceiveIGTLMessage(buffer, false))
      {
      this->Content.image = DefaultVideoDevice->GetContent().image;
      this->Modified();
      return 1;
      }
    }
  else if (strcmp(headerMsg->GetDeviceType(), igtlio::VideoConverter::GetIGTLTypeName()) == 0)
    {
    igtlio::VideoDevice* videoDevice = reinterpret_cast<igtlio::VideoDevice*>(this->LinkedDevice.GetPointer());
    if (videoDevice)
      {
      if (this->LinkedDevice->ReceiveIGTLMessage(buffer, checkCRC))
        {
        this->Content.image = videoDevice->GetContent().image;
        this->Modified();
        return 1;
        }
      }
    }
  return 0;
}


//---------------------------------------------------------------------------

vtkSmartPointer<vtkUnsignedCharArray> vtkIGTLStreamingVolumeCodec::GetCompressedStreamFromData()
{
  return this->GetStreamFromContentUsingDefaultDevice();
}

vtkSmartPointer<vtkUnsignedCharArray> vtkIGTLStreamingVolumeCodec::GetStreamFromContentUsingDefaultDevice()
{
  if (!Content.image)
    {
    vtkWarningMacro("Image is NULL, message not generated.")
    return NULL;
    }
  if (Content.image->GetDimensions()[0] == 0 ||
      Content.image->GetDimensions()[1] == 0 ||
      Content.image->GetDimensions()[2] == 0)
    {
    vtkWarningMacro("Image dimensions wrong, message not generated.")
    return NULL;
    }
  igtlio::VideoConverter::ContentData deviceContent = DefaultVideoDevice->GetContent();
  deviceContent.image = Content.image;
  DefaultVideoDevice->SetContent(deviceContent);
  igtl::VideoMessage::Pointer videoMessage = dynamic_pointer_cast<igtl::VideoMessage>(DefaultVideoDevice->GetIGTLMessage());
  if (videoMessage.GetPointer() == NULL)
    {
    vtkWarningMacro("Encoding failed, message not generated.")
    return NULL;
    }
  this->CopyVideoMessageIntoFrameMSG(videoMessage);
  if(DefaultVideoDevice->GetContent().keyFrameUpdated)
    {
    this->CopyVideoMessageIntoKeyFrameMSG(videoMessage);
    }
  this->Content.keyFrameUpdated = DefaultVideoDevice->GetContent().keyFrameUpdated;
  return Content.frame;
}

//---------------------------------------------------------------------------
void vtkIGTLStreamingVolumeCodec::PrintSelf(ostream& os, vtkIndent indent)
{
  Superclass::PrintSelf(os, indent);
  os << indent << "Video:\t" <<"\n";
  Content.image->PrintSelf(os, indent.GetNextIndent());
}
