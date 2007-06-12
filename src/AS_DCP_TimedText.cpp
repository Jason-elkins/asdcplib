/*
Copyright (c) 2007, John Hurst
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions
are met:
1. Redistributions of source code must retain the above copyright
   notice, this list of conditions and the following disclaimer.
2. Redistributions in binary form must reproduce the above copyright
   notice, this list of conditions and the following disclaimer in the
   documentation and/or other materials provided with the distribution.
3. The name of the author may not be used to endorse or promote products
   derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/
/*! \file    AS_DCP_TimedText.cpp
    \version $Id$       
    \brief   AS-DCP library, PCM essence reader and writer implementation
*/


#include "AS_DCP_internal.h"
#include "AS_DCP_TimedText.h"
#include "KM_xml.h"

static std::string TIMED_TEXT_PACKAGE_LABEL = "File Package: SMPTE 429-5 frame wrapping of D-Cinema Timed Text data";
static std::string TIMED_TEXT_DEF_LABEL = "Timed Text Track";


//------------------------------------------------------------------------------------------

const char*
MIME2str(TimedText::MIMEType_t m)
{
  if ( m == TimedText::MT_PNG )
    return "image/png";

  else if ( m == TimedText::MT_OPENTYPE )
    return "application/x-opentype";

  return "application/octet-stream";
}

//
void
ASDCP::TimedText::DescriptorDump(ASDCP::TimedText::TimedTextDescriptor const& TDesc, FILE* stream)
{
  if ( stream == 0 )
    stream = stderr;

  UUID TmpID(TDesc.AssetID);
  char buf[64];

  fprintf(stream, "         EditRate: %u/%u\n", TDesc.EditRate.Numerator, TDesc.EditRate.Denominator);
  fprintf(stream, "ContainerDuration: %u\n",    TDesc.ContainerDuration);
  fprintf(stream, "          AssetID: %s\n",    TmpID.EncodeHex(buf, 64));
  fprintf(stream, "    NamespaceName: %s\n", TDesc.NamespaceName.c_str());
  fprintf(stream, "    ResourceCount: %lu\n", TDesc.ResourceList.size());

  TimedText::ResourceList_t::const_iterator ri;
  for ( ri = TDesc.ResourceList.begin() ; ri != TDesc.ResourceList.end(); ri++ )
    {
      TmpID.Set((*ri).ResourceID);
      fprintf(stream, "    %s: %s\n",
	      TmpID.EncodeHex(buf, 64), 
	      MIME2str((*ri).Type));
    }
}

//
void
ASDCP::TimedText::FrameBuffer::Dump(FILE* stream, ui32_t dump_len) const
{
  if ( stream == 0 )
    stream = stderr;

  UUID TmpID(m_AssetID);
  char buf[64];
  fprintf(stream, "%s | %s | %u\n", TmpID.EncodeHex(buf, 64), m_MIMEType.c_str(), Size());

  if ( dump_len > 0 )
    Kumu::hexdump(m_Data, dump_len, stream);
}

//------------------------------------------------------------------------------------------

typedef std::map<UUID, UUID> ResourceMap_t;

class ASDCP::TimedText::MXFReader::h__Reader : public ASDCP::h__Reader
{
  TimedTextDescriptor*  m_EssenceDescriptor;
  ResourceMap_t         m_ResourceMap;

  ASDCP_NO_COPY_CONSTRUCT(h__Reader);

public:
  TimedTextDescriptor m_TDesc;    

  h__Reader() : m_EssenceDescriptor(0) {
    memset(&m_TDesc.AssetID, 0, UUIDlen);
  }

  Result_t    OpenRead(const char*);
  Result_t    MD_to_TimedText_TDesc(TimedText::TimedTextDescriptor& TDesc);
  Result_t    ReadTimedTextResource(FrameBuffer& FrameBuf, AESDecContext* Ctx, HMACContext* HMAC);
  Result_t    ReadAncillaryResource(const byte_t*, FrameBuffer& FrameBuf, AESDecContext* Ctx, HMACContext* HMAC);
};

//
ASDCP::Result_t
ASDCP::TimedText::MXFReader::h__Reader::MD_to_TimedText_TDesc(TimedText::TimedTextDescriptor& TDesc)
{
  assert(m_EssenceDescriptor);
  memset(&m_TDesc.AssetID, 0, UUIDlen);
  MXF::DCTimedTextDescriptor* TDescObj = (MXF::DCTimedTextDescriptor*)m_EssenceDescriptor;

  TDesc.EditRate = TDescObj->SampleRate;
  TDesc.ContainerDuration = TDescObj->ContainerDuration;
  TDesc.NamespaceName = TDescObj->RootNamespaceName;
  TDesc.EncodingName = TDescObj->UTFEncoding;

  Batch<UUID>::const_iterator sdi = TDescObj->SubDescriptors.begin();
  DCTimedTextResourceDescriptor* DescObject = 0;
  Result_t result = RESULT_OK;

  for ( ; sdi != TDescObj->SubDescriptors.end() && KM_SUCCESS(result); sdi++ )
    {
      result = m_HeaderPart.GetMDObjectByID(*sdi, (InterchangeObject**)&DescObject);

      if ( KM_SUCCESS(result) )
	{
	  TimedTextResourceDescriptor TmpResource;
	  memcpy(TmpResource.ResourceID, DescObject->ResourcePackageID.Value(), UUIDlen);

	  if ( DescObject->ResourceMIMEType.find("font/") != std::string::npos )
	    TmpResource.Type = MT_OPENTYPE;

	  else if ( DescObject->ResourceMIMEType.find("image/png") != std::string::npos )
	    TmpResource.Type = MT_PNG;

	  else
	    TmpResource.Type = MT_BIN;

	  TDesc.ResourceList.push_back(TmpResource);
	  m_ResourceMap.insert(ResourceMap_t::value_type(DescObject->ResourcePackageID, *sdi));
	}
      else
	{
	  DefaultLogSink().Error("Broken sub-descriptor link\n");
	  return RESULT_FORMAT;
	}
    }

  return RESULT_OK;
}

//
ASDCP::Result_t
ASDCP::TimedText::MXFReader::h__Reader::OpenRead(char const* filename)
{
  Result_t result = OpenMXFRead(filename);
  
  if( ASDCP_SUCCESS(result) )
    {
      if ( m_EssenceDescriptor == 0 )
	  m_HeaderPart.GetMDObjectByType(OBJ_TYPE_ARGS(DCTimedTextDescriptor), (InterchangeObject**)&m_EssenceDescriptor);

      result = MD_to_TimedText_TDesc(m_TDesc);
    }

  if( ASDCP_SUCCESS(result) )
    result = InitMXFIndex();

  if( ASDCP_SUCCESS(result) )
    result = InitInfo();

  if( ASDCP_SUCCESS(result) )
    memcpy(m_TDesc.AssetID, m_Info.AssetUUID, UUIDlen);

  return result;
}

//
ASDCP::Result_t
ASDCP::TimedText::MXFReader::h__Reader::ReadTimedTextResource(FrameBuffer& FrameBuf,
							      AESDecContext* Ctx, HMACContext* HMAC)
{
  if ( ! m_File.IsOpen() )
    return RESULT_INIT;

  return ReadEKLVFrame(0, FrameBuf, Dict::ul(MDD_DCTimedTextEssence), Ctx, HMAC);
}

//
ASDCP::Result_t
ASDCP::TimedText::MXFReader::h__Reader::ReadAncillaryResource(const byte_t* uuid, FrameBuffer& FrameBuf,
							      AESDecContext* Ctx, HMACContext* HMAC)
{
  KM_TEST_NULL_L(uuid);
  UUID RID(uuid);

  ResourceMap_t::const_iterator ri = m_ResourceMap.find(RID);
  if ( ri == m_ResourceMap.end() )
    {
      char buf[64];
      DefaultLogSink().Error("No such resource: %s\n", RID.EncodeHex(buf, 64));
      return RESULT_FORMAT;
    }

  DCTimedTextResourceDescriptor* DescObject = 0;
  // get the subdescriptor
  Result_t result = m_HeaderPart.GetMDObjectByID((*ri).second, (InterchangeObject**)&DescObject);

  if ( KM_SUCCESS(result) )
    {
      Array<RIP::Pair>::const_iterator pi;
      RIP::Pair TmpPair;
      ui32_t sequence = 1;

      // Look up the partition start in the RIP using the SID.
      // Count the sequence length in because this is the sequence
      // value needed to  complete the HMAC.
      for ( pi = m_HeaderPart.m_RIP.PairArray.begin(); pi != m_HeaderPart.m_RIP.PairArray.end(); pi++, sequence++ )
	{
	  if ( (*pi).BodySID == DescObject->ResourceSID )
	    {
	      TmpPair = *pi;
	      break;
	    }
	}

      if ( TmpPair.ByteOffset == 0 )
	{
	  DefaultLogSink().Error("Body SID not found in RIP set: %d\n", DescObject->ResourceSID);
	  return RESULT_FORMAT;
	}

      if ( KM_SUCCESS(result) )
	{
	  FrameBuf.AssetID(uuid);
	  FrameBuf.MIMEType(DescObject->ResourceMIMEType);

	  // seek tp the start of the partition
	  if ( (Kumu::fpos_t)TmpPair.ByteOffset != m_LastPosition )
	    {
	      m_LastPosition = TmpPair.ByteOffset;
	      result = m_File.Seek(TmpPair.ByteOffset);
	    }

	  // read the partition header
	  MXF::Partition GSPart;
	  result = GSPart.InitFromFile(m_File);

	  if( ASDCP_SUCCESS(result) )
	    {
	      // check the SID
	      if ( DescObject->ResourceSID != GSPart.BodySID )
		{
		  char buf[64];
		  DefaultLogSink().Error("Generic stream partition body differs: %s\n", RID.EncodeHex(buf, 64));
		  return RESULT_FORMAT;
		}

	      // read the essence packet
	      if( ASDCP_SUCCESS(result) )
		result = ReadEKLVPacket(0, FrameBuf, Dict::ul(MDD_DCTimedTextDescriptor), Ctx, HMAC);
	    }
	}
    }

  return result;
}


//------------------------------------------------------------------------------------------

ASDCP::TimedText::MXFReader::MXFReader()
{
  m_Reader = new h__Reader;
}


ASDCP::TimedText::MXFReader::~MXFReader()
{
}

// Open the file for reading. The file must exist. Returns error if the
// operation cannot be completed.
ASDCP::Result_t
ASDCP::TimedText::MXFReader::OpenRead(const char* filename) const
{
  return m_Reader->OpenRead(filename);
}

// Fill the struct with the values from the file's header.
// Returns RESULT_INIT if the file is not open.
ASDCP::Result_t
ASDCP::TimedText::MXFReader::FillDescriptor(TimedText::TimedTextDescriptor& TDesc) const
{
  if ( m_Reader && m_Reader->m_File.IsOpen() )
    {
      TDesc = m_Reader->m_TDesc;
      return RESULT_OK;
    }

  return RESULT_INIT;
}

// Fill the struct with the values from the file's header.
// Returns RESULT_INIT if the file is not open.
ASDCP::Result_t
ASDCP::TimedText::MXFReader::FillWriterInfo(WriterInfo& Info) const
{
  if ( m_Reader && m_Reader->m_File.IsOpen() )
    {
      Info = m_Reader->m_Info;
      return RESULT_OK;
    }

  return RESULT_INIT;
}

//
ASDCP::Result_t
ASDCP::TimedText::MXFReader::ReadTimedTextResource(std::string& s, AESDecContext* Ctx, HMACContext* HMAC) const
{
  FrameBuffer FrameBuf(2*Kumu::Megabyte);

  Result_t result = ReadTimedTextResource(FrameBuf, Ctx, HMAC);

  if ( ASDCP_SUCCESS(result) )
    s.assign((char*)FrameBuf.Data(), FrameBuf.Size());

  return result;
}

//
ASDCP::Result_t
ASDCP::TimedText::MXFReader::ReadTimedTextResource(FrameBuffer& FrameBuf,
						   AESDecContext* Ctx, HMACContext* HMAC) const
{
  if ( m_Reader && m_Reader->m_File.IsOpen() )
    return m_Reader->ReadTimedTextResource(FrameBuf, Ctx, HMAC);

  return RESULT_INIT;
}

//
ASDCP::Result_t
ASDCP::TimedText::MXFReader::ReadAncillaryResource(const byte_t* uuid, FrameBuffer& FrameBuf,
						   AESDecContext* Ctx, HMACContext* HMAC) const
{
  if ( m_Reader && m_Reader->m_File.IsOpen() )
    return m_Reader->ReadAncillaryResource(uuid, FrameBuf, Ctx, HMAC);

  return RESULT_INIT;
}


//
void
ASDCP::TimedText::MXFReader::DumpHeaderMetadata(FILE* stream) const
{
  if ( m_Reader->m_File.IsOpen() )
    m_Reader->m_HeaderPart.Dump(stream);
}


//
void
ASDCP::TimedText::MXFReader::DumpIndex(FILE* stream) const
{
  if ( m_Reader->m_File.IsOpen() )
    m_Reader->m_FooterPart.Dump(stream);
}

//------------------------------------------------------------------------------------------


//
class ASDCP::TimedText::MXFWriter::h__Writer : public ASDCP::h__Writer
{
public:
  TimedTextDescriptor m_TDesc;
  byte_t              m_EssenceUL[SMPTE_UL_LENGTH];
  ui32_t              m_ResourceSID;

  ASDCP_NO_COPY_CONSTRUCT(h__Writer);

  h__Writer() : m_ResourceSID(10) {
    memset(m_EssenceUL, 0, SMPTE_UL_LENGTH);
  }

  ~h__Writer(){}

  Result_t OpenWrite(const char*, ui32_t HeaderSize);
  Result_t SetSourceStream(const TimedTextDescriptor&);
  Result_t WriteTimedTextResource(const std::string& XMLDoc, AESEncContext* = 0, HMACContext* = 0);
  Result_t WriteAncillaryResource(const FrameBuffer&, AESEncContext* = 0, HMACContext* = 0);
  Result_t Finalize();
  Result_t TimedText_TDesc_to_MD(TimedText::TimedTextDescriptor& TDesc);
};

//
ASDCP::Result_t
ASDCP::TimedText::MXFWriter::h__Writer::TimedText_TDesc_to_MD(TimedText::TimedTextDescriptor& TDesc)
{
  assert(m_EssenceDescriptor);
  MXF::DCTimedTextDescriptor* TDescObj = (MXF::DCTimedTextDescriptor*)m_EssenceDescriptor;

  TDescObj->SampleRate = TDesc.EditRate;
  TDescObj->ContainerDuration = TDesc.ContainerDuration;
  TDescObj->RootNamespaceName = TDesc.NamespaceName;
  TDescObj->UTFEncoding = TDesc.EncodingName;

  return RESULT_OK;
}

//
ASDCP::Result_t
ASDCP::TimedText::MXFWriter::h__Writer::OpenWrite(char const* filename, ui32_t HeaderSize)
{
  if ( ! m_State.Test_BEGIN() )
    return RESULT_STATE;

  Result_t result = m_File.OpenWrite(filename);

  if ( ASDCP_SUCCESS(result) )
    {
      m_HeaderSize = HeaderSize;
      m_EssenceDescriptor = new DCTimedTextDescriptor();
      result = m_State.Goto_INIT();
    }

  return result;
}

//
ASDCP::Result_t
ASDCP::TimedText::MXFWriter::h__Writer::SetSourceStream(ASDCP::TimedText::TimedTextDescriptor const& TDesc)
{
  if ( ! m_State.Test_INIT() )
    return RESULT_STATE;

  m_TDesc = TDesc;
  ResourceList_t::const_iterator ri;
  Result_t result = TimedText_TDesc_to_MD(m_TDesc);

  for ( ri = m_TDesc.ResourceList.begin() ; ri != m_TDesc.ResourceList.end() && ASDCP_SUCCESS(result); ri++ )
    {
      DCTimedTextResourceDescriptor* resourceSubdescriptor = new DCTimedTextResourceDescriptor;
      GenRandomValue(resourceSubdescriptor->InstanceUID);
      resourceSubdescriptor->ResourcePackageID.Set((*ri).ResourceID);
      resourceSubdescriptor->ResourceMIMEType = MIME2str((*ri).Type);
      resourceSubdescriptor->ResourceSID = m_ResourceSID++;
      m_EssenceSubDescriptorList.push_back((FileDescriptor*)resourceSubdescriptor);
      m_EssenceDescriptor->SubDescriptors.push_back(resourceSubdescriptor->InstanceUID);
    }

  m_ResourceSID = 10;

  if ( ASDCP_SUCCESS(result) )
    {
      UMID SourcePackageUMID;
      SourcePackageUMID.MakeUMID(0x0f, m_TDesc.AssetID);

      InitHeader();
      AddDMSegment(m_TDesc.EditRate, 24, TIMED_TEXT_DEF_LABEL,
		   UL(Dict::ul(MDD_PictureDataDef)), TIMED_TEXT_PACKAGE_LABEL, SourcePackageUMID);

      AddEssenceDescriptor(UL(Dict::ul(MDD_DCTimedTextWrapping)));

      result = m_HeaderPart.WriteToFile(m_File, m_HeaderSize);
      
      if ( KM_SUCCESS(result) )
	result = CreateBodyPart(m_TDesc.EditRate);
    }

  if ( ASDCP_SUCCESS(result) )
    {
      memcpy(m_EssenceUL, Dict::ul(MDD_DCTimedTextEssence), SMPTE_UL_LENGTH);
      m_EssenceUL[SMPTE_UL_LENGTH-1] = 1; // first (and only) essence container
      result = m_State.Goto_READY();
    }

  return result;
}

//
ASDCP::Result_t
ASDCP::TimedText::MXFWriter::h__Writer::WriteTimedTextResource(const std::string& XMLDoc,
							       ASDCP::AESEncContext* Ctx, ASDCP::HMACContext* HMAC)
{
  Result_t result = m_State.Goto_RUNNING();

  if ( ASDCP_SUCCESS(result) )
    {
      // TODO: make sure it's XML

      ui32_t str_size = XMLDoc.size();
      FrameBuffer FrameBuf(str_size);
      
      memcpy(FrameBuf.Data(), XMLDoc.c_str(), str_size);
      FrameBuf.Size(str_size);

      IndexTableSegment::IndexEntry Entry;
      Entry.StreamOffset = m_StreamOffset;
      
      if ( ASDCP_SUCCESS(result) )
	result = WriteEKLVPacket(FrameBuf, m_EssenceUL, Ctx, HMAC);

      if ( ASDCP_SUCCESS(result) )
	{
	  m_FooterPart.PushIndexEntry(Entry);
	  m_FramesWritten++;
	}
    }

  return result;
}


//
ASDCP::Result_t
ASDCP::TimedText::MXFWriter::h__Writer::WriteAncillaryResource(const ASDCP::TimedText::FrameBuffer& FrameBuf,
							       ASDCP::AESEncContext* Ctx, ASDCP::HMACContext* HMAC)
{
  if ( ! m_State.Test_RUNNING() )
    return RESULT_STATE;

  Kumu::fpos_t here = m_File.Tell();

  // create generic stream partition header
  MXF::Partition GSPart;

  GSPart.ThisPartition = here;
  GSPart.PreviousPartition = m_HeaderPart.m_RIP.PairArray.back().ByteOffset;
  GSPart.BodySID = m_ResourceSID;
  GSPart.OperationalPattern = m_HeaderPart.OperationalPattern;

  m_HeaderPart.m_RIP.PairArray.push_back(RIP::Pair(m_ResourceSID++, here));
  GSPart.EssenceContainers.push_back(UL(Dict::ul(MDD_DCTimedTextEssence)));
  UL TmpUL(Dict::ul(MDD_GenericStreamPartition));
  Result_t result = GSPart.WriteToFile(m_File, TmpUL);

  if ( ASDCP_SUCCESS(result) )
    result = WriteEKLVPacket(FrameBuf, m_EssenceUL, Ctx, HMAC);

 m_FramesWritten++;
  return result;
}

//
ASDCP::Result_t
ASDCP::TimedText::MXFWriter::h__Writer::Finalize()
{
  if ( ! m_State.Test_RUNNING() )
    return RESULT_STATE;

  m_FramesWritten = m_TDesc.ContainerDuration;
  m_State.Goto_FINAL();

  return WriteMXFFooter();
}


//------------------------------------------------------------------------------------------

ASDCP::TimedText::MXFWriter::MXFWriter()
{
}

ASDCP::TimedText::MXFWriter::~MXFWriter()
{
}


// Open the file for writing. The file must not exist. Returns error if
// the operation cannot be completed.
ASDCP::Result_t
ASDCP::TimedText::MXFWriter::OpenWrite(const char* filename, const WriterInfo& Info,
				       const TimedTextDescriptor& TDesc, ui32_t HeaderSize)
{
  if ( Info.LabelSetType != LS_MXF_SMPTE )
    {
      DefaultLogSink().Error("Timed Text support requires LS_MXF_SMPTE\n");
      return RESULT_FORMAT;
    }

  m_Writer = new h__Writer;
  
  Result_t result = m_Writer->OpenWrite(filename, HeaderSize);

  if ( ASDCP_SUCCESS(result) )
    {
      m_Writer->m_Info = Info;
      result = m_Writer->SetSourceStream(TDesc);
    }

  if ( ASDCP_FAILURE(result) )
    m_Writer.release();

  return result;
}

//
ASDCP::Result_t
ASDCP::TimedText::MXFWriter::WriteTimedTextResource(const std::string& XMLDoc, AESEncContext* Ctx, HMACContext* HMAC)
{
  if ( m_Writer.empty() )
    return RESULT_INIT;

  return m_Writer->WriteTimedTextResource(XMLDoc, Ctx, HMAC);
}

//
ASDCP::Result_t
ASDCP::TimedText::MXFWriter::WriteAncillaryResource(const FrameBuffer& FrameBuf, AESEncContext* Ctx, HMACContext* HMAC)
{
  if ( m_Writer.empty() )
    return RESULT_INIT;

  return m_Writer->WriteAncillaryResource(FrameBuf, Ctx, HMAC);
}

// Closes the MXF file, writing the index and other closing information.
ASDCP::Result_t
ASDCP::TimedText::MXFWriter::Finalize()
{
  if ( m_Writer.empty() )
    return RESULT_INIT;

  return m_Writer->Finalize();
}



//
// end AS_DCP_timedText.cpp
//
