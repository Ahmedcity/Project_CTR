#include "lib.h"

#include "srl.h"
#include "ncsd_read.h"
#include "ncch_read.h"
#include "exheader_read.h"
#include "exefs_read.h"

#include "cia_build.h"
#include "cia_read.h"
#include "tik_build.h"
#include "tmd_build.h"
#include "titleid.h"
#include "certs.h"

const int CIA_ALIGN_SIZE = 0x40;
const int CIA_CONTENT_ALIGN = 0x10;

// Private Prototypes
void InitCiaSettings(cia_settings *set);
void FreeCiaSettings(cia_settings *set);
int GetCiaSettings(cia_settings *ciaset, user_settings *usrset);

int GetSettingsFromUsrset(cia_settings *ciaset, user_settings *usrset);
int GetSettingsFromNcch0(cia_settings *ciaset, u32 ncch0_offset);
int GetTmdDataFromNcch(cia_settings *ciaset, u8 *ncch, ncch_info *ncch_ctx, u8 *key);
int GetMetaRegion(cia_settings *ciaset, u8 *ncch, ncch_info *ncch_ctx, u8 *key);
int GetContentFilePtrs(cia_settings *ciaset, user_settings *usrset);
int ImportNcchContent(cia_settings *ciaset);
int GetSettingsFromSrl(cia_settings *ciaset);
int GetSettingsFromCci(cia_settings *ciaset);

u16 SetupVersion(u16 major, u16 minor, u16 micro);

void GetContentHashes(cia_settings *ciaset);
void EncryptContent(cia_settings *ciaset);

int BuildCiaCertChain(cia_settings *ciaset);
int BuildCiaHdr(cia_settings *ciaset);

int WriteCiaToFile(cia_settings *ciaset);


int build_CIA(user_settings *usrset)
{
	int result = 0;

	// Init Settings
	cia_settings *ciaset = calloc(1,sizeof(cia_settings));
	if(!ciaset) {
		fprintf(stderr,"[CIA ERROR] Not enough memory\n"); 
		return MEM_ERROR;
	}

	// Get Settings
	InitCiaSettings(ciaset);
	result = GetCiaSettings(ciaset,usrset);
	if(result) goto finish;

	// Create Output File
	ciaset->out = fopen(usrset->common.outFileName,"wb");
	if(!ciaset->out){
		fprintf(stderr,"[CIA ERROR] Failed to create \"%s\"\n",usrset->common.outFileName);
		result = FAILED_TO_CREATE_OUTFILE;
		goto finish;
	}

	// Create CIA Sections

	/* Certificate Chain */
	result = BuildCiaCertChain(ciaset);
	if(result) goto finish;

	/* Ticket */
	result = BuildTicket(ciaset);
	if(result) goto finish;

	/* Title Metadata */
	result = BuildTMD(ciaset);
	if(result) goto finish;

	/* CIA Header */
	result = BuildCiaHdr(ciaset);
	if(result) goto finish;
	
	/* Write To File */
	result = WriteCiaToFile(ciaset);
	if(result) goto finish;

finish:
	if(result != FAILED_TO_CREATE_OUTFILE && ciaset->out) 
		fclose(ciaset->out);

	FreeCiaSettings(ciaset);

	return result;
}

void InitCiaSettings(cia_settings *set)
{
	memset(set,0,sizeof(cia_settings));
}

void FreeCiaSettings(cia_settings *set)
{
	if(set->content.filePtrs){
		for(u32 i = 1; i < set->content.count; i++){
			fclose(set->content.filePtrs[i]);
		}
		free(set->content.filePtrs);
	}
	free(set->ciaSections.certChain.buffer);
	free(set->ciaSections.tik.buffer);
	free(set->ciaSections.tmd.buffer);
	free(set->ciaSections.meta.buffer);
	free(set->ciaSections.content.buffer);

	memset(set,0,sizeof(cia_settings));

	free(set);
}

int GetCiaSettings(cia_settings *ciaset, user_settings *usrset)
{
	int result = 0;

	// Transfering data from usrset
	result = GetSettingsFromUsrset(ciaset,usrset);

	if(usrset->common.workingFileType == infile_ncch){
		if((result = GetSettingsFromNcch0(ciaset,0)) != 0) 
			return result;
		if((result = GetContentFilePtrs(ciaset,usrset)) != 0) 
			return result;
		if((result = ImportNcchContent(ciaset)) != 0) 
			return result;
	}

	else if(usrset->common.workingFileType == infile_srl){
		if((result = GetSettingsFromSrl(ciaset)) != 0) 
			return result;
	}

	else if(usrset->common.workingFileType == infile_ncsd){
		if((result = GetSettingsFromCci(ciaset)) != 0) 
			return result;
	}
	
	GetContentHashes(ciaset);

	if(ciaset->content.encryptCia)
		EncryptContent(ciaset);

	return 0;
}

int GetSettingsFromUsrset(cia_settings *ciaset, user_settings *usrset)
{
	// General Stuff
	ciaset->keys = &usrset->common.keys;
	ciaset->rsf = &usrset->common.rsfSet;
	ciaset->ciaSections.content.buffer = usrset->common.workingFile.buffer;
	ciaset->ciaSections.content.size = usrset->common.workingFile.size;
	usrset->common.workingFile.buffer = NULL;
	ciaset->ciaSections.content.size = 0;
	ciaset->content.includeUpdateNcch = usrset->cia.includeUpdateNcch;
	ciaset->verbose = usrset->common.verbose;
	
	u32_to_u8(ciaset->tmd.titleType,TYPE_CTR,BE);
	ciaset->content.encryptCia = usrset->common.rsfSet.Option.EnableCrypt;
	ciaset->content.IsDlc = usrset->cia.DlcContent;
	if(ciaset->keys->aes.commonKey[ciaset->keys->aes.currentCommonKey] == NULL && ciaset->content.encryptCia){
		fprintf(stderr,"[CIA WARNING] Common Key could not be loaded, CIA will not be encrypted\n");
		ciaset->content.encryptCia = false;
	}
	
	ciaset->cert.caCrlVersion = 0;
	ciaset->cert.signerCrlVersion = 0;

	for(int i = 0; i < 3; i++){
		ciaset->common.titleVersion[i] = usrset->cia.titleVersion[i];
	}

	// Ticket Data
	rndset(ciaset->tik.ticketId,8);
	u32_to_u8(ciaset->tik.deviceId,usrset->cia.deviceId,BE);
	u32_to_u8(ciaset->tik.eshopAccId,usrset->cia.eshopAccId,BE);
	ciaset->tik.licenceType = 0;
	ciaset->tik.audit = 0;
	
	if(usrset->cia.randomTitleKey)
		rndset(ciaset->common.titleKey,AES_128_KEY_SIZE);
	else
		clrmem(ciaset->common.titleKey,AES_128_KEY_SIZE);

	if(ciaset->verbose)
		memdump(stdout,"[CIA] CIA title key: ",ciaset->common.titleKey,AES_128_KEY_SIZE);
		
	ciaset->tik.formatVersion = 1;

	int result = GenCertChildIssuer(ciaset->tik.issuer,ciaset->keys->certs.xsCert);
	if(result) return result;
	
	// Tmd Stuff
	if(usrset->cia.contentId[0] > MAX_U32)
		ciaset->content.id[0] = u32GetRand();
	else 
		ciaset->content.id[0] = usrset->cia.contentId[0];

	ciaset->tmd.formatVersion = 1;
	result = GenCertChildIssuer(ciaset->tmd.issuer,ciaset->keys->certs.cpCert);
	return 0;
}

int GetSettingsFromNcch0(cia_settings *ciaset, u32 ncch0_offset)
{
	/* Sanity Checks */
	if(!ciaset->ciaSections.content.buffer) 
		return CIA_NO_NCCH0;

	u8 *ncch0 = (u8*)(ciaset->ciaSections.content.buffer+ncch0_offset);

	if(!IsNcch(NULL,ncch0)){
		fprintf(stderr,"[CIA ERROR] Content0 is not NCCH\n");
		return CIA_INVALID_NCCH0;
	}

	/* Get Ncch0 Header */
	ncch_hdr *hdr = (ncch_hdr*)ncch0;
	ciaset->content.IsCfa = IsCfa(hdr);

	ciaset->content.offset[0] = 0;
	ciaset->content.size[0] = align(GetNcchSize(hdr),0x10);
	ciaset->content.totalSize = ciaset->content.size[0];

	/* Get Ncch0 Import Context */
	ncch_info *info = calloc(1,sizeof(ncch_info));
	if(!info){ 
		fprintf(stderr,"[CIA ERROR] Not enough memory\n"); 
		return MEM_ERROR; 
	}
	GetNcchInfo(info,hdr);

	/* Verify Ncch0 (Sig&Hash Checks) */
	int result = VerifyNcch(ncch0,ciaset->keys,false,true);
	if(result == UNABLE_TO_LOAD_NCCH_KEY){
		ciaset->content.keyNotFound = true;
		if(!ciaset->content.IsCfa){
			fprintf(stderr,"[CIA WARNING] CXI AES Key could not be loaded\n");
			fprintf(stderr,"      Meta Region, SaveDataSize, Remaster Version cannot be obtained\n");
		}
	}
	else if(result != 0){
		fprintf(stderr,"[CIA ERROR] Content 0 Is Corrupt (res = %d)\n",result);
		return CIA_INVALID_NCCH0;
	}

	/* Gen Settings From Ncch0 */
	endian_memcpy(ciaset->common.titleId,hdr->titleId,8,LE);


	/* Getting ncch key */
	u8 *ncchkey = NULL;
	if(!ciaset->content.keyNotFound || IsNcchEncrypted(hdr)){
		SetNcchKeys(ciaset->keys,hdr);
		ncchkey = ciaset->keys->aes.ncchKey0;
		if(ciaset->verbose){
			printf("[CIA] NCCH AES keys:\n");
			memdump(stdout," > key0: ",ciaset->keys->aes.ncchKey0,AES_128_KEY_SIZE);
			memdump(stdout," > key1: ",ciaset->keys->aes.ncchKey1,AES_128_KEY_SIZE);
		}
	}

	/* Get TMD Data from ncch */
	result = GetTmdDataFromNcch(ciaset,ncch0,info,ncchkey); // Data For TMD
	if(result) goto finish;
	/* Get META Region from ncch */
	result = GetMetaRegion(ciaset,ncch0,info,ncchkey); // Meta Region
	/* Finish */
finish:
	/* Return */
	free(info);
	return result;	
}

int GetTmdDataFromNcch(cia_settings *ciaset, u8 *ncch, ncch_info *ncch_ctx, u8 *key)
{
	extended_hdr *exhdr = malloc(sizeof(extended_hdr));
	memcpy(exhdr,ncch+ncch_ctx->exhdrOffset,sizeof(extended_hdr));
	if(key != NULL)
		CryptNcchRegion((u8*)exhdr,sizeof(extended_hdr),0,ncch_ctx,key,ncch_exhdr);

	u16 Category = u8_to_u16((ciaset->common.titleId+2),BE);
	if(IsPatch(Category)||ciaset->content.IsCfa||ciaset->content.keyNotFound) 
		u32_to_u8(ciaset->tmd.savedataSize,0,LE);
	else 
		u32_to_u8(ciaset->tmd.savedataSize,(u32)GetSaveDataSize_frm_exhdr(exhdr),LE);
		
	if(ciaset->rsf->SystemControlInfo.SaveDataSize && !ciaset->content.IsCfa && ciaset->content.keyNotFound){
		u64 size = 0;
		GetSaveDataSizeFromString(&size,ciaset->rsf->SystemControlInfo.SaveDataSize,"CIA");
		u32_to_u8(ciaset->tmd.savedataSize,(u32)size,LE);
	}
	
	if(ciaset->content.IsCfa||ciaset->content.keyNotFound){
		if(ciaset->common.titleVersion[VER_MAJOR] == MAX_U16){ // '-major' wasn't set
			if(ciaset->content.IsCfa){ // Is a CFA and can be decrypted
				fprintf(stderr,"[CIA ERROR] Invalid major version. Use \"-major\" option.\n");
				return CIA_BAD_VERSION;
			}
			else // CXI which cannot be decrypted
				ciaset->common.titleVersion[VER_MAJOR] = 0;
		}
	}
	else{ // Is a CXI and can be decrypted
		if(ciaset->common.titleVersion[VER_MAJOR] != MAX_U16){ // '-major' was set
			fprintf(stderr,"[CIA ERROR] Option \"-major\" cannot be applied for cxi.\n");
			return CIA_BAD_VERSION;
		}
		// Setting remaster ver
		ciaset->common.titleVersion[VER_MAJOR] = GetRemasterVersion_frm_exhdr(exhdr);
	}

	ciaset->tmd.version = ciaset->tik.version = SetupVersion(ciaset->common.titleVersion[VER_MAJOR],ciaset->common.titleVersion[VER_MINOR],ciaset->common.titleVersion[VER_MICRO]);

	free(exhdr);
	return 0;
}

int GetMetaRegion(cia_settings *ciaset, u8 *ncch, ncch_info *info, u8 *key)
{
	if(ciaset->content.IsCfa || ciaset->content.keyNotFound) 
		return 0;

	extended_hdr *exhdr = malloc(sizeof(extended_hdr));
	memcpy(exhdr,ncch+info->exhdrOffset,sizeof(extended_hdr));
	if(key != NULL)
		CryptNcchRegion((u8*)exhdr,sizeof(extended_hdr),0,info,key,ncch_exhdr);

	exefs_hdr *exefsHdr = malloc(sizeof(exefs_hdr));
	memcpy(exefsHdr,ncch+info->exefsOffset,sizeof(exefs_hdr));
	if(key != NULL)
		CryptNcchRegion((u8*)exefsHdr,sizeof(exefs_hdr),0,info,key,ncch_exefs);

	u32 icon_size = 0;
	u32 icon_offset = 0;
	for(int i = 0; i < MAX_EXEFS_SECTIONS; i++){
		if(strncmp(exefsHdr->fileHdr[i].name,"icon",8) == 0){
			icon_size = u8_to_u32(exefsHdr->fileHdr[i].size,LE);
			icon_offset = u8_to_u32(exefsHdr->fileHdr[i].offset,LE) + sizeof(exefs_hdr);
		}
	}
	
	ciaset->ciaSections.meta.size = sizeof(cia_metadata) + icon_size;
	ciaset->ciaSections.meta.buffer = malloc(ciaset->ciaSections.meta.size);
	if(!ciaset->ciaSections.meta.buffer){
		fprintf(stderr,"[CIA ERROR] Not enough memory\n");
		return MEM_ERROR; 
	}
	cia_metadata *hdr = (cia_metadata*)ciaset->ciaSections.meta.buffer;
	memset(hdr,0,sizeof(cia_metadata));
	GetDependencyList_frm_exhdr(hdr->dependencyList,exhdr);
	GetCoreVersion_frm_exhdr(hdr->coreVersion,exhdr);
	if(icon_size > 0){
		u8 *IconDestPos = (ciaset->ciaSections.meta.buffer + sizeof(cia_metadata));
		memcpy(IconDestPos,ncch+info->exefsOffset+icon_offset,icon_size);
		if(key != NULL)
			CryptNcchRegion(IconDestPos,icon_size,icon_offset,info,key,ncch_exefs);
		//memdump(stdout,"Icon: ",IconDestPos,0x10);
	}

	free(exefsHdr);
	free(exhdr);
	return 0;
}

int GetContentFilePtrs(cia_settings *ciaset, user_settings *usrset)
{
	ciaset->content.filePtrs = malloc(sizeof(FILE*)*CIA_MAX_CONTENT);
	if(!ciaset->content.filePtrs){
		fprintf(stderr,"[CIA ERROR] Not enough memory\n"); 
		return MEM_ERROR; 
	}
	memset(ciaset->content.filePtrs,0,sizeof(FILE*)*CIA_MAX_CONTENT);
	int j = 1;
	ncch_hdr *hdr = malloc(sizeof(ncch_hdr));
	for(int i = 1; i < CIA_MAX_CONTENT; i++){
		if(usrset->common.contentPath[i]){
			if(!AssertFile(usrset->common.contentPath[i])){ 
				fprintf(stderr,"[CIA ERROR] Failed to open \"%s\"\n",usrset->common.contentPath[i]); 
				return FAILED_TO_OPEN_FILE; 
			}
			ciaset->content.fileSize[j] = GetFileSize64(usrset->common.contentPath[i]);
			ciaset->content.filePtrs[j] = fopen(usrset->common.contentPath[i],"rb");
			
			if(usrset->cia.contentId[i] > MAX_U32)
				ciaset->content.id[j] = u32GetRand(); 
			else 
				ciaset->content.id[j] = (u32)usrset->cia.contentId[i];

			ciaset->content.index[j] = (u16)i;

			// Get Data from ncch HDR
			ReadNcchHdr(hdr,ciaset->content.filePtrs[j]);
			
			// Get Size
			u64 calcSize = GetNcchSize(hdr);
			if(calcSize != ciaset->content.fileSize[j]){
				fprintf(stderr,"[CIA ERROR] \"%s\" is corrupt\n",usrset->common.contentPath[i]); 
				return FAILED_TO_OPEN_FILE; 
			}

			ciaset->content.size[j] = align(calcSize,CIA_CONTENT_ALIGN);
			ciaset->content.offset[j] = ciaset->content.totalSize;
			
			ciaset->content.totalSize += ciaset->content.size[j];
			

			// Finish get next content
			j++;
		}
	}
	free(hdr);
	ciaset->content.count = j;

	// Check Conflicting IDs
	for(int i = 0; i < ciaset->content.count; i++){
		for(j = i+1; j < ciaset->content.count; j++){
			if(ciaset->content.id[j] == ciaset->content.id[i]){
				fprintf(stderr,"[CIA ERROR] CIA Content %d and %d, have conflicting IDs\n",ciaset->content.index[j],ciaset->content.index[i]);
				return CIA_CONFILCTING_CONTENT_IDS;
			}
		}
	}
	return 0;
}

int ImportNcchContent(cia_settings *ciaset)
{
	ciaset->ciaSections.content.buffer = realloc(ciaset->ciaSections.content.buffer,ciaset->content.totalSize);
	if(!ciaset->ciaSections.content.buffer){
		fprintf(stderr,"[CIA ERROR] Not enough memory\n");
		return MEM_ERROR;
	}

	ncch_hdr *ncch0hdr = (ncch_hdr*)ciaset->ciaSections.content.buffer;
	for(int i = 1; i < ciaset->content.count; i++){
		// Import
		u8 *ncchpos = (u8*)(ciaset->ciaSections.content.buffer+ciaset->content.offset[i]);

		ReadFile64(ncchpos, ciaset->content.fileSize[i], 0, ciaset->content.filePtrs[i]);
		if(ModifyNcchIds(ncchpos, NULL, ncch0hdr->programId, ciaset->keys) != 0)
			return -1;
		
		// Set Additional Flags
		if(ciaset->content.IsDlc)
			ciaset->content.flags[i] |= content_Optional;

		//if(unknown condition)
		//	ciaset->content.flags[i] |= content_Shared;
	}

	ciaset->ciaSections.content.size = ciaset->content.totalSize;
	return 0;
}

int GetSettingsFromSrl(cia_settings *ciaset)
{
	srl_hdr *hdr = (srl_hdr*)ciaset->ciaSections.content.buffer;
	if(!hdr || ciaset->ciaSections.content.size < sizeof(srl_hdr)) {
		fprintf(stderr,"[CIA ERROR] Invalid TWL SRL File\n");
		return FAILED_TO_IMPORT_FILE;
	}
	
	// Check if TWL SRL File
	if(u8_to_u16(&hdr->title_id[6],LE) != 0x0003){
		fprintf(stderr,"[CIA ERROR] Invalid TWL SRL File\n");
		return FAILED_TO_IMPORT_FILE;
	}

	// Generate and store Converted TitleID
	u64_to_u8(ciaset->common.titleId,ConvertTwlIdToCtrId(u8_to_u64(hdr->title_id,LE)),BE);
	//memdump(stdout,"SRL TID: ",ciaset->TitleID,8);

	// Get TWL Flag
	ciaset->tmd.twlFlag = ((hdr->reserved_flags[3] & 6) >> 1);

	// Get Remaster Version
	u16 version = SetupVersion(hdr->romVersion,ciaset->common.titleVersion[1],0);
	ciaset->tik.version = version;
	ciaset->tmd.version = version;

	// Get SaveDataSize (Public and Private)
	memcpy(ciaset->tmd.savedataSize,hdr->pubSaveDataSize,4);
	memcpy(ciaset->tmd.privSavedataSize,hdr->privSaveDataSize,4);

	// Setting CIA Content Settings
	ciaset->content.count = 1;
	ciaset->content.offset[0] = 0;
	ciaset->content.size[0] = ciaset->ciaSections.content.size;
	ciaset->content.totalSize = ciaset->ciaSections.content.size;

	return 0;
}

int GetSettingsFromCci(cia_settings *ciaset)
{
	int result = 0;

	if(!IsCci(ciaset->ciaSections.content.buffer)){
		fprintf(stderr,"[CIA ERROR] Invalid CCI file\n");
		return FAILED_TO_IMPORT_FILE;
	}
	
	u32 ncch0_offset = GetPartitionOffset(ciaset->ciaSections.content.buffer,0);
	if(!ncch0_offset){
		fprintf(stderr,"[CIA ERROR] Invalid CCI file (invalid ncch0)\n");
		return FAILED_TO_IMPORT_FILE;
	}

	result = GetSettingsFromNcch0(ciaset, ncch0_offset);
	if(result){
		fprintf(stderr,"Import of Ncch 0 failed(%d)\n",result);	
		return result;
	}
	int j = 1;
	
	u64 cciContentOffsets[CCI_MAX_CONTENT];
	cciContentOffsets[0] = ncch0_offset;
	for(int i = 1; i < 8; i++){
		if(GetPartitionSize(ciaset->ciaSections.content.buffer,i)){
			ncch_hdr *ncchHdr = (ncch_hdr*)GetPartition(ciaset->ciaSections.content.buffer, i);
			
			if(IsUpdateCfa(ncchHdr) && !ciaset->content.includeUpdateNcch)
				continue;
			
			cciContentOffsets[j] = GetPartitionOffset(ciaset->ciaSections.content.buffer,i);
			
			// Get Size
			ciaset->content.size[j] =  GetPartitionSize(ciaset->ciaSections.content.buffer,i);
			ciaset->content.offset[j] = ciaset->content.totalSize;
			
			ciaset->content.totalSize += ciaset->content.size[j];
			
			// Get ID
			ciaset->content.id[j] = u32GetRand();

			// Get Index
			ciaset->content.index[j] = i;

			// Increment Content Count
			j++;
		}
	}
	ciaset->content.count = j;

	for(int i = 0; i < ciaset->content.count; i++){ // Re-organising content positions in memory
		u8 *cci_pos = (ciaset->ciaSections.content.buffer + cciContentOffsets[i]);
		u8 *cia_pos = (ciaset->ciaSections.content.buffer + ciaset->content.offset[i]);
		memcpy(cia_pos,cci_pos,ciaset->content.size[i]);
	}
	ciaset->ciaSections.content.size = ciaset->content.totalSize;
	return 0;
}

u16 SetupVersion(u16 major, u16 minor, u16 micro)
{
	return (((major << 10) & 0xFC00) | ((minor << 4) & 0x3F0) | (micro & 0xf));
}

void GetContentHashes(cia_settings *ciaset)
{
	for(int i = 0; i < ciaset->content.count; i++)
		ctr_sha(ciaset->ciaSections.content.buffer+ciaset->content.offset[i],ciaset->content.size[i],ciaset->content.hash[i],CTR_SHA_256);
}

void EncryptContent(cia_settings *ciaset)
{
	for(int i = 0; i < ciaset->content.count; i++){
		ciaset->content.flags[i] |= content_Encrypted;
		u8 *content = ciaset->ciaSections.content.buffer+ciaset->content.offset[i];
		CryptContent(content, content, ciaset->content.size[i], ciaset->common.titleKey, i, ENC);
	}
}

int BuildCiaCertChain(cia_settings *ciaset)
{
	ciaset->ciaSections.certChain.size = GetCertSize(ciaset->keys->certs.caCert) + GetCertSize(ciaset->keys->certs.xsCert) + GetCertSize(ciaset->keys->certs.cpCert);
	ciaset->ciaSections.certChain.buffer = malloc(ciaset->ciaSections.certChain.size);
	if(!ciaset->ciaSections.certChain.buffer) {
		fprintf(stderr,"[CIA ERROR] Not enough memory\n");
		return MEM_ERROR; 
	}
	memcpy(ciaset->ciaSections.certChain.buffer,ciaset->keys->certs.caCert,GetCertSize(ciaset->keys->certs.caCert));
	memcpy((ciaset->ciaSections.certChain.buffer+GetCertSize(ciaset->keys->certs.caCert)),ciaset->keys->certs.xsCert,GetCertSize(ciaset->keys->certs.xsCert));
	memcpy((ciaset->ciaSections.certChain.buffer+GetCertSize(ciaset->keys->certs.caCert)+GetCertSize(ciaset->keys->certs.xsCert)),ciaset->keys->certs.cpCert,GetCertSize(ciaset->keys->certs.cpCert));
	return 0;
}

int BuildCiaHdr(cia_settings *ciaset)
{
	// Allocating memory for header
	ciaset->ciaSections.ciaHdr.size = sizeof(cia_hdr);
	ciaset->ciaSections.ciaHdr.buffer = malloc(ciaset->ciaSections.ciaHdr.size);
	if(!ciaset->ciaSections.ciaHdr.buffer){
		fprintf(stderr,"[CIA ERROR] Not enough memory\n");
		return MEM_ERROR;
	}
	
	cia_hdr *hdr = (cia_hdr*)ciaset->ciaSections.ciaHdr.buffer;

	// Clearing 
	memset(hdr,0,sizeof(cia_hdr));

	// Setting Data
	u32_to_u8(hdr->hdrSize,sizeof(cia_hdr),LE);
	u16_to_u8(hdr->type,0x0,LE);
	u16_to_u8(hdr->version,0x0,LE);
	u32_to_u8(hdr->certChainSize,ciaset->ciaSections.certChain.size,LE);
	u32_to_u8(hdr->tikSize,ciaset->ciaSections.tik.size,LE);
	u32_to_u8(hdr->tmdSize,ciaset->ciaSections.tmd.size,LE);
	u32_to_u8(hdr->metaSize,ciaset->ciaSections.meta.size,LE);
	u64_to_u8(hdr->contentSize,ciaset->content.totalSize,LE);

	// Recording Offsets
	ciaset->ciaSections.certChainOffset = align(sizeof(cia_hdr),0x40);
	ciaset->ciaSections.tikOffset = align(ciaset->ciaSections.certChainOffset+ciaset->ciaSections.certChain.size,0x40);
	ciaset->ciaSections.tmdOffset = align(ciaset->ciaSections.tikOffset+ciaset->ciaSections.tik.size,0x40);
	ciaset->ciaSections.contentOffset = align(ciaset->ciaSections.tmdOffset+ciaset->ciaSections.tmd.size,0x40);
	ciaset->ciaSections.metaOffset = align(ciaset->ciaSections.contentOffset+ciaset->content.totalSize,0x40);
	
	for(int i = 0; i < ciaset->content.count; i++)
		hdr->contentIndex[ciaset->content.index[i]/8] |= 1 << (7 - (ciaset->content.index[i] & 7));
		
	return 0;
}

int WriteCiaToFile(cia_settings *ciaset)
{
	WriteBuffer(ciaset->ciaSections.ciaHdr.buffer,ciaset->ciaSections.ciaHdr.size,0,ciaset->out);
	WriteBuffer(ciaset->ciaSections.certChain.buffer,ciaset->ciaSections.certChain.size,ciaset->ciaSections.certChainOffset,ciaset->out);
	WriteBuffer(ciaset->ciaSections.tik.buffer,ciaset->ciaSections.tik.size,ciaset->ciaSections.tikOffset,ciaset->out);
	WriteBuffer(ciaset->ciaSections.tmd.buffer,ciaset->ciaSections.tmd.size,ciaset->ciaSections.tmdOffset,ciaset->out);
	WriteBuffer(ciaset->ciaSections.content.buffer,ciaset->ciaSections.content.size,ciaset->ciaSections.contentOffset,ciaset->out);
	WriteBuffer(ciaset->ciaSections.meta.buffer,ciaset->ciaSections.meta.size,ciaset->ciaSections.metaOffset,ciaset->out);
	return 0;
}


int CryptContent(u8 *enc, u8 *dec, u64 size, u8 *title_key, u16 index, u8 mode)
{
	//generating IV
	u8 iv[16];
	memset(&iv,0x0,16);
	iv[0] = (index >> 8) & 0xff;
	iv[1] = index & 0xff;
	//Crypting content
	ctr_aes_context ctx;
	memset(&ctx,0x0,sizeof(ctr_aes_context));
	ctr_init_aes_cbc(&ctx,title_key,iv,mode);
	if(mode == ENC) ctr_aes_cbc(&ctx,dec,enc,size,ENC);
	else ctr_aes_cbc(&ctx,enc,dec,size,DEC);
	return 0;
}

bool IsCia(u8 *cia)
{
	if(!cia)
		return false;
	
	cia_hdr *hdr = (cia_hdr*)cia;
	
	if(u8_to_u32(hdr->hdrSize,LE) != sizeof(cia_hdr) || u8_to_u16(hdr->type,LE) != 0 || u8_to_u16(hdr->version,LE) != 0)
		return false;
	
	if(!GetCiaCertSize(hdr) || !GetCiaTikSize(hdr) || !GetCiaTmdSize(hdr) || !GetCiaContentSize(hdr))
		return false;
		
	return true;
}

u64 GetCiaCertOffset(cia_hdr *hdr)
{
	u64 hdrSize = u8_to_u32(hdr->hdrSize,LE);
	return align(hdrSize,CIA_ALIGN_SIZE);
}

u64 GetCiaCertSize(cia_hdr *hdr)
{
	return u8_to_u32(hdr->certChainSize,LE);
}

u64 GetCiaTikOffset(cia_hdr *hdr)
{
	u64 certOffset = GetCiaCertOffset(hdr);
	u64 certSize = GetCiaCertSize(hdr);
	return align(certOffset + certSize,CIA_ALIGN_SIZE);
}

u64 GetCiaTikSize(cia_hdr *hdr)
{
	return u8_to_u32(hdr->tikSize,LE);
}

u64 GetCiaTmdOffset(cia_hdr *hdr)
{
	u64 tikOffset = GetCiaTikOffset(hdr);
	u64 tikSize = GetCiaTikSize(hdr);
	return align(tikOffset + tikSize,CIA_ALIGN_SIZE);
}

u64 GetCiaTmdSize(cia_hdr *hdr)
{
	return u8_to_u32(hdr->tmdSize,LE);
}

u64 GetCiaContentOffset(cia_hdr *hdr)
{
	u64 tmdOffset = GetCiaTmdOffset(hdr);
	u64 tmdSize = GetCiaTmdSize(hdr);
	return align(tmdOffset + tmdSize,CIA_ALIGN_SIZE);
}

u64 GetCiaContentSize(cia_hdr *hdr)
{
	return u8_to_u64(hdr->contentSize,LE);
}

u64 GetCiaMetaOffset(cia_hdr *hdr)
{
	u64 contentOffset = GetCiaContentOffset(hdr);
	u64 contentSize = GetCiaContentSize(hdr);
	return align(contentOffset + contentSize,CIA_ALIGN_SIZE);
}

u64 GetCiaMetaSize(cia_hdr *hdr)
{
	return u8_to_u32(hdr->metaSize,LE);
}

u8* GetCiaCert(u8 *cia)
{
	return cia + GetCiaCertOffset((cia_hdr*)cia);
}

u8* GetCiaTik(u8 *cia)
{
	return cia + GetCiaTikOffset((cia_hdr*)cia);
}

u8* GetCiaTmd(u8 *cia)
{
	return cia + GetCiaTmdOffset((cia_hdr*)cia);
}

u8* GetCiaContent(u8 *cia)
{
	return cia + GetCiaContentOffset((cia_hdr*)cia);
}

u8* GetCiaMeta(u8 *cia)
{
	if(GetCiaMetaSize((cia_hdr*)cia))
		return cia + GetCiaMetaOffset((cia_hdr*)cia);
	else
		return NULL;
}