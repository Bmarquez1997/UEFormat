#include "Archive/UEFormatReader.h"

#include "Misc/Compression.h"
#include "Misc/FileHelper.h"
#include "zstd.h"

FUEFormatReader::FUEFormatReader(TArray<uint8> InData)
{
	Storage = MakeShared<TArray<uint8>>(MoveTemp(InData));
	Limit = Storage->Num();
}

bool FUEFormatReader::AtEnd() const
{
	return Offset >= Limit;
}

int32 FUEFormatReader::Tell() const
{
	return Offset;
}

int32 FUEFormatReader::Remaining() const
{
	return Limit - Offset;
}

bool FUEFormatReader::CanRead(int32 Count) const
{
	return Storage.IsValid() && Count >= 0 && Offset + Count <= Limit;
}

void FUEFormatReader::Seek(int32 Position)
{
	Offset = FMath::Clamp(Position, 0, Limit);
}

void FUEFormatReader::Skip(int32 Count)
{
	Offset = FMath::Clamp(Offset + Count, 0, Limit);
}

FUEFormatReader FUEFormatReader::Chunk(int32 ByteSize)
{
	ByteSize = FMath::Clamp(ByteSize, 0, Remaining());

	FUEFormatReader Result;
	Result.Storage = Storage;
	Result.Origin = Origin + Offset;
	Result.Limit = ByteSize;
	Result.Offset = 0;
	Result.FileVersion = FileVersion;
	Offset += ByteSize;
	return Result;
}

FUEFormatReader FUEFormatReader::ReadRemaining()
{
	return Chunk(Remaining());
}

const uint8* FUEFormatReader::Cursor() const
{
	return Storage.IsValid() ? Storage->GetData() + Origin + Offset : nullptr;
}

bool FUEFormatReader::ReadBool()
{
	return Read<uint8>() != 0;
}

uint8 FUEFormatReader::ReadByte()
{
	return Read<uint8>();
}

int32 FUEFormatReader::ReadInt()
{
	return Read<int32>();
}

int16 FUEFormatReader::ReadShort()
{
	return Read<int16>();
}

uint16 FUEFormatReader::ReadUShort()
{
	return Read<uint16>();
}

float FUEFormatReader::ReadFloat()
{
	return Read<float>();
}

FVector3f FUEFormatReader::ReadVector()
{
	return Read<FVector3f>();
}

FQuat4f FUEFormatReader::ReadQuat()
{
	const float X = Read<float>();
	const float Y = Read<float>();
	const float Z = Read<float>();
	const float W = Read<float>();
	return FQuat4f(X, Y, Z, W).GetNormalized();
}

std::string FUEFormatReader::ReadFString()
{
	return ReadFixedString(ReadInt());
}

std::string FUEFormatReader::ReadFixedString(int32 Size)
{
	if (Size <= 0)
	{
		return {};
	}
	if (!CanRead(Size))
	{
		UE_LOG(LogTemp, Error, TEXT("UEFormat: read past end of buffer"));
		return {};
	}

	std::string Result(reinterpret_cast<const char*>(Cursor()), Size);
	Offset += Size;
	return Result;
}

TArray<uint8> FUEFormatReader::ReadBytes(int32 Count)
{
	TArray<uint8> Bytes;
	if (Count <= 0)
	{
		return Bytes;
	}
	if (!CanRead(Count))
	{
		UE_LOG(LogTemp, Error, TEXT("UEFormat: read past end of buffer"));
		return Bytes;
	}

	Bytes.Append(Cursor(), Count);
	Offset += Count;
	return Bytes;
}

bool UEFormat::LoadFile(const FString& Filename, FUEFormatHeader& OutHeader, FUEFormatReader& OutBody)
{
	TArray<uint8> FileData;
	if (!FFileHelper::LoadFileToArray(FileData, *Filename))
	{
		UE_LOG(LogTemp, Error, TEXT("Failed to load UEFormat file: %s"), *Filename);
		return false;
	}

	FUEFormatReader Ar(MoveTemp(FileData));

	const std::string Magic = Ar.ReadFixedString(8);
	if (Magic != "UEFORMAT")
	{
		return false;
	}

	OutHeader.Identifier = Ar.ReadFString();
	OutHeader.FileVersion = static_cast<EUEFormatVersion>(Ar.ReadByte());
	if (OutHeader.FileVersion > EUEFormatVersion::LatestVersion)
	{
		UE_LOG(
			LogTemp,
			Error,
			TEXT("File Version %d is not supported for this version of the importer."),
			static_cast<int32>(OutHeader.FileVersion));
		return false;
	}

	OutHeader.ObjectName = Ar.ReadFString();
	if (OutHeader.FileVersion >= EUEFormatVersion::AttributeFormatRestructure)
	{
		OutHeader.ObjectPath = Ar.ReadFString();
	}

	OutHeader.bIsCompressed = Ar.ReadBool();
	if (OutHeader.bIsCompressed)
	{
		OutHeader.CompressionType = Ar.ReadFString();
		OutHeader.UncompressedSize = Ar.ReadInt();
		OutHeader.CompressedSize = Ar.ReadInt();

		TArray<uint8> CompressedBuffer = Ar.ReadBytes(OutHeader.CompressedSize);
		if (CompressedBuffer.Num() != OutHeader.CompressedSize)
		{
			UE_LOG(LogTemp, Error, TEXT("Error reading compressed data."));
			return false;
		}

		TArray<uint8> UncompressedBuffer;
		UncompressedBuffer.SetNumUninitialized(OutHeader.UncompressedSize);

		if (OutHeader.CompressionType == "ZSTD")
		{
			ZSTD_decompress(
				UncompressedBuffer.GetData(),
				OutHeader.UncompressedSize,
				CompressedBuffer.GetData(),
				OutHeader.CompressedSize);
		}
		else if (OutHeader.CompressionType == "GZIP")
		{
			FCompression::UncompressMemory(
				NAME_Gzip,
				UncompressedBuffer.GetData(),
				OutHeader.UncompressedSize,
				CompressedBuffer.GetData(),
				OutHeader.CompressedSize);
		}
		else
		{
			UE_LOG(LogTemp, Error, TEXT("Unknown Compression Type: %s"), UTF8_TO_TCHAR(OutHeader.CompressionType.c_str()));
			return false;
		}

		OutBody = FUEFormatReader(MoveTemp(UncompressedBuffer));
	}
	else
	{
		OutBody = Ar.ReadRemaining();
	}

	OutBody.FileVersion = OutHeader.FileVersion;
	return true;
}
