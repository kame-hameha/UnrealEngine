// Copyright 1998-2015 Epic Games, Inc. All Rights Reserved.

#if WITH_BUILDPATCHGENERATION

#include "BuildPatchServicesPrivatePCH.h"
#include "FileAttributesParser.h"

namespace BuildPatchServices
{
	class FFileAttributesParserImpl
		: public FFileAttributesParser
	{
		typedef void(*SetKeyValueAttributeFunction)(FFileAttributes&, FString);

	public:
		FFileAttributesParserImpl(IFileManager* FileManager);
		virtual ~FFileAttributesParserImpl();

		virtual bool ParseFileAttributes(const FString& MetaFilename, TMap<FString, FFileAttributes>& FileAttributes) override;

	private:
		bool FileAttributesMetaToMap(const FString& AttributesList, TMap<FString, FFileAttributes>& FileAttributesMap);

	private:
		IFileManager* FileManager;
		TMap<FString, SetKeyValueAttributeFunction> AttributeSetters;
	};

	FFileAttributesParserImpl::FFileAttributesParserImpl(IFileManager* InFileManager)
		: FileManager(InFileManager)
	{
		AttributeSetters.Add(TEXT("readonly"),   [](FFileAttributes& Attributes, FString Value){ Attributes.bReadOnly = true; });
		AttributeSetters.Add(TEXT("compressed"), [](FFileAttributes& Attributes, FString Value){ Attributes.bCompressed = true; });
		AttributeSetters.Add(TEXT("executable"), [](FFileAttributes& Attributes, FString Value){ Attributes.bUnixExecutable = true; });
		AttributeSetters.Add(TEXT("tag"),        [](FFileAttributes& Attributes, FString Value){ Attributes.InstallTags.Add(MoveTemp(Value)); });
	}

	FFileAttributesParserImpl::~FFileAttributesParserImpl()
	{
	}

	bool FFileAttributesParserImpl::ParseFileAttributes(const FString& MetaFilename, TMap<FString, FFileAttributes>& FileAttributes)
	{
		FArchive* Reader = FileManager->CreateFileReader(*MetaFilename);
		if (Reader != nullptr)
		{
			TArray<uint8> FileData;
			FileData.AddUninitialized(Reader->TotalSize());
			Reader->Serialize(FileData.GetData(), FileData.Num());
			bool Successful = Reader->Close();
			delete Reader;
			if (Successful)
			{
				FString FileDataString;
				FFileHelper::BufferToString(FileDataString, FileData.GetData(), FileData.Num());
				return FileAttributesMetaToMap(FileDataString, FileAttributes);
			}
			else
			{
				UE_LOG(LogBuildPatchServices, Error, TEXT("FFileAttributesParser: Error occured reading meta file %s"), *MetaFilename);
			}
		}
		else
		{
			UE_LOG(LogBuildPatchServices, Error, TEXT("FFileAttributesParser: Could not open meta file %s"), *MetaFilename);
		}

		return false;
	}

	bool FFileAttributesParserImpl::FileAttributesMetaToMap(const FString& AttributesList, TMap<FString, FFileAttributes>& FileAttributesMap)
	{
		const TCHAR Quote = TEXT('\"');
		const TCHAR EOFile = TEXT('\0');
		const TCHAR EOLine = TEXT('\n');

		bool Successful = true;
		bool FoundFilename = false;

		const TCHAR* CharPtr = *AttributesList;
		while (*CharPtr != EOFile)
		{
			// Parse filename
			while (*CharPtr != Quote && *CharPtr != EOFile){ ++CharPtr; }
			if (*CharPtr == EOFile)
			{
				if (!FoundFilename)
				{
					UE_LOG(LogBuildPatchServices, Error, TEXT("FFileAttributesParser: Did not find opening quote for filename!"));
					return false;
				}
				break;
			}
			const TCHAR* FilenameStart = ++CharPtr;
			while (*CharPtr != Quote && *CharPtr != EOFile && *CharPtr != EOLine){ ++CharPtr; }
			// Check we didn't run out of file
			if (*CharPtr == EOFile)
			{
				UE_LOG(LogBuildPatchServices, Error, TEXT("FFileAttributesParser: Unexpected end of file before next quote! Pos:%d"), CharPtr - *AttributesList);
				return false;
			}
			// Check we didn't run out of line
			if(*CharPtr == EOLine)
			{
				UE_LOG(LogBuildPatchServices, Error, TEXT("FFileAttributesParser: Unexpected end of line before next quote! Pos:%d"), CharPtr - *AttributesList);
				return false;
			}
			// Save positions
			const TCHAR* FilenameEnd = CharPtr++;
			const TCHAR* AttributesStart = CharPtr;
			// Parse keywords
			while (*CharPtr != Quote && *CharPtr != EOFile && *CharPtr != EOLine){ ++CharPtr; }
			// Check we hit the end of the line or file, another quote it wrong
			if (*CharPtr == Quote)
			{
				UE_LOG(LogBuildPatchServices, Error, TEXT("FFileAttributesParser: Unexpected Quote before end of keywords! Pos:%d"), CharPtr - *AttributesList);
				return false;
			}
			FoundFilename = true;
			// Save position
			const TCHAR* EndOfLine = CharPtr;
			// Grab info
			FString Filename = FString(FilenameEnd - FilenameStart, FilenameStart).Replace(TEXT("\\"), TEXT("/"));
			FFileAttributes& FileAttributes = FileAttributesMap.FindOrAdd(Filename);
			TArray<FString> AttributeParamsArray;
			FString AttributeParams(EndOfLine - AttributesStart, AttributesStart);
			AttributeParams.ParseIntoArrayWS(AttributeParamsArray);
			for (const FString& AttributeParam : AttributeParamsArray)
			{
				FString Key, Value;
				if(!AttributeParam.Split(TEXT(":"), &Key, &Value))
				{
					Key = AttributeParam;
				}
				if (AttributeSetters.Contains(Key))
				{
					AttributeSetters[Key](FileAttributes, MoveTemp(Value));
				}
				else
				{
					UE_LOG(LogBuildPatchServices, Error, TEXT("FFileAttributesParser: Unrecognised attribute %s for %s"), *AttributeParam, *Filename);
					Successful = false;
				}
			}
		}

		return Successful;
	}

	FFileAttributesParserRef FFileAttributesParserFactory::Create(IFileManager* FileManager)
	{
		return MakeShareable(new FFileAttributesParserImpl(FileManager));
	}
}

#endif
