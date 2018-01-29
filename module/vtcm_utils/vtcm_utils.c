#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <limits.h>
#include <dirent.h>
#include <sys/time.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <openssl/rsa.h>
#include <openssl/evp.h>
#include <string.h>
#include "data_type.h"
#include "errno.h"
#include "alloc.h"
#include "list.h"
#include "attrlist.h"
#include "memfunc.h"
#include "basefunc.h"
#include "struct_deal.h"
#include "crypto_func.h"
#include "memdb.h"
#include "message.h"
#include "ex_module.h"
#include "sys_func.h"
#include "tcm_constants.h"
#include "vtcm_utils.h"
#include "app_struct.h"
#include "pik_struct.h"
#include "sm3.h"
#include "sm4.h"

static  BYTE Buf[DIGEST_SIZE*32];
static  BYTE Output[DIGEST_SIZE*32];
Record_List sessions_list;
TCM_PUBKEY *pubEK;
TCM_SECRET ownerAuth;
TCM_SECRET smkAuth;
Record_List entitys_list;

// Ex CA Module

extern BYTE * CAprikey;
extern unsigned long * CAprilen;
extern BYTE * CApubkey;

void * vtcm_auto_build_outputmsg(char * out_line, void * active_msg)
{ 

   struct tcm_utils_output * output_para;
   int offset=0;
   int ret;
   int i=0;
   void * send_msg;
   BYTE * out_param;	
   

   output_para=Talloc0(sizeof(*output_para));
   if(output_para==NULL)
	return NULL;

   do{
	out_param=&Output[DIGEST_SIZE*2*i];
   	Memset(out_param,0,DIGEST_SIZE*32);
	ret=Getfiledfromstr(out_param,out_line+offset,' ',DIGEST_SIZE*2);
	if(ret>0)
	{
		i++;
		offset+=ret;
	}
    }while(ret>0);

// Build output para message

   output_para->param_num=i;
   if(i>0)
   {	
   	output_para->params=Talloc0(DIGEST_SIZE*2*output_para->param_num);	
	Memcpy(output_para->params,Output,DIGEST_SIZE*2*output_para->param_num); 

   }
   send_msg=message_create(DTYPE_VTCM_UTILS,SUBTYPE_TCM_UTILS_OUTPUT,active_msg);
   message_add_record(send_msg,output_para);
   return send_msg;	
}


TCM_KEY *global_tcm_key;
BYTE *global_DecryptData=NULL;
int keyLength=0;
int sm2Length=0;
struct tcm_entity_appinfo
{
    BYTE uuid[DIGEST_SIZE];
    TCM_HANDLE handle;
    TCM_ENT_TYPE type;
    TCM_SECRET ownerAuth;
    TCM_SECRET migAuth;
    int data_size;
    BYTE * data;
}__attribute__((packed));
void print_bin_data(BYTE * data,int len,int width)
{
    int i;
    for(i=0;i<len;i++){
        printf("%.2x ",data[i]);
        if (width>0)
        { 	
            if((i+1)%width==0)
                printf("\n");
        }
    }
    printf("\n");
}
TCM_SESSION_DATA * Create_AuthSession_Data(TCM_ENT_TYPE * type,BYTE * auth,BYTE * nonce)
{
    TCM_SESSION_DATA * authdata=Dalloc0(sizeof(*authdata),NULL);
    if(authdata==NULL)
        return NULL;
    authdata->entityTypeByte=*type;
    Memcpy(authdata->nonceEven,nonce,TCM_HASH_SIZE);
    Memcpy(authdata->sharedSecret,auth,TCM_HASH_SIZE);
    return authdata;	
}

TCM_AUTHHANDLE Build_AuthSession(TCM_SESSION_DATA * authdata,void * tcm_out_data)
{
    BYTE auth[TCM_HASH_SIZE];
    struct tcm_out_APCreate * apcreate_out = tcm_out_data;
    authdata->SERIAL=apcreate_out->sernum;
    // Build shareSecret
    Memcpy(Buf,authdata->nonceEven,TCM_HASH_SIZE);
    Memcpy(authdata->nonceEven,apcreate_out->nonceEven,TCM_HASH_SIZE);
    Memcpy(Buf+TCM_HASH_SIZE,apcreate_out->nonceEven,TCM_HASH_SIZE);
    Memcpy(auth,authdata->sharedSecret,TCM_HASH_SIZE);
    //memset(auth,0,TCM_HASH_SIZE);
    sm3_hmac(auth,TCM_HASH_SIZE,Buf,TCM_HASH_SIZE*2,authdata->sharedSecret);

    if(authdata->entityTypeByte!=TCM_ET_NONE)
    {
        //	check the authcode

    }
    authdata->handle=apcreate_out->authHandle;
    // add authdata to the session_list

    Record_List * record = Calloc0(sizeof(*record));
    if(record==NULL)
        return -EINVAL;
    INIT_LIST_HEAD(&record->list);
    record->record=authdata;
    List_add_tail(&record->list,&sessions_list.list);
    return authdata->handle;	
}

TCM_SESSION_DATA * Find_AuthSession(TCM_ENT_TYPE type, TCM_AUTHHANDLE authhandle)
{
    Record_List * record;
    Record_List * head;
    struct List_head * curr;
    TCM_SESSION_DATA * authdata;

    head=&(sessions_list.list);
    curr=head->list.next;

    while(curr!=head)
    {
        record=List_entry(curr,Record_List,list);
        authdata=record->record;
        if(authdata==NULL)
            return NULL;
        if(type==0)
        {
            if(authdata->handle==authhandle)
                return authdata;
        }

        if(authdata->entityTypeByte==type)
        {
            if(type==TCM_ET_NONE)
                return authdata;
            if(authdata->handle==authhandle)
                return authdata;
        }
        curr=curr->next;
    }
    return NULL;
}


int proc_vtcmutils_input(void * sub_proc,void * recv_msg);

// proceed the vtcm command
int proc_vtcmutils_ChangeAuth(void * sub_proc,void * para);
int proc_vtcmutils_SM2Encrypt(void * sub_proc, void * para); 
int proc_vtcmutils_SM2Decrypt(void * sub_proc, void * para); 
int proc_vtcmutils_SM4Encrypt(void * sub_proc, void * para); 
int proc_vtcmutils_SM4Decrypt(void * sub_proc, void * para); 
int proc_vtcmutils_NV_ReadValue(void *sub_proc,void *para);
int proc_vtcmutils_NV_WriteValue(void * sub_proc,void *para);
int proc_vtcmutils_NV_DefineSpace(void * sub_proc, void * para);
int proc_vtcmutils_getcapability(void * sub_proc, void * para);
int proc_vtcmutils_SelfTestFull(void * sub_proc,void * para);
int proc_vtcmutils_ContinueSelfTest(void *sub_proc,void *para);
int proc_vtcmutils_GetTestResult(void *sub_proc,void *para);
int proc_vtcmutils_ForceClear(void *sub_proc,void *para);
int proc_vtcmutils_DisableForceClear(void *sub_proc,void *para);
int proc_vtcmutils_SM3CompleteExtend(void * sub_proc, void * para); 
int proc_vtcmutils_SM3Update(void * sub_proc, void * para); 
int proc_vtcmutils_SM3Start(void * sub_proc, void * para); 
int proc_vtcmutils_SM3Complete(void * sub_proc, void * para); 
int proc_vtcmutils_StartUp(void * sub_proc, void * para); 
int proc_vtcmutils_PhysicalSetDeactivated(void * sub_proc, void * para); 
int proc_vtcmutils_getRandom(void * sub_proc, void * para); 
int proc_vtcmutils_takeownership(void * sub_proc, void * para);
int proc_vtcmutils_PhysicalEnable(void * sub_proc, void * para);
int proc_vtcmutils_PhysicalDisable(void * sub_proc, void * para);
int vtcmutils_transmit(int in_len,BYTE * in, int *  out_len, BYTE * out);
int proc_vtcmutils_readPubek(void * sub_proc, void * para);
int proc_vtcmutils_createEKPair(void * sub_proc, void * para);
int proc_vtcmutils_PcrRead(void * sub_proc,void * para);
int proc_vtcmutils_Extend(void * sub_proc,void * para);
int proc_vtcmutils_PcrReset(void *sub_proc,void * para);
int proc_vtcmutils_APCreate(void * sub_proc, void * para);
int proc_vtcmutils_APTerminate(void *sub_proc,void * para);
int proc_vtcm_utils_OwnerClear(void *sub_proc,void *para);
int proc_vtcmutils_DisableOwnerClear(void *sub_proc ,void * para);
int proc_vtcmutils_FlushSpecific(void *sub_proc,void * para);
int proc_vtcmutils_OwnerReadInternalPub(void *sub_proc,void *para);
int proc_vtcmutils_CertifyKey(void *sub_proc,void *para);
int proc_vtcmutils_CreateWrapKey(void *sub_proc,void *para);
int proc_vtcmutils_LoadKey(void *sub_proc,void *para);
int proc_vtcmutils_MakeIdentity(void *sub_proc,void *para);
int proc_vtcmutils_Quote(void *sub_proc,void *para);
int proc_vtcmutils_Seal(void *sub_proc,void *para);
int proc_vtcmutils_Unseal(void *sub_proc,void *para);
int proc_vtcmutils_Sign(void *sub_proc,void *para);

//int proc_vtcmutils_ExCreateSm2Key(void * sub_proc,void * para);
//int proc_vtcmutils_ExLoadCAKey(void * sub_proc,void * para);
//int proc_vtcmutils_ExCaSign(void * sub_proc,void * para);
//int proc_vtcmutils_Ex
int proc_vtcmutils_WrapKey(void *sub_proc,void *para);

int vtcm_SM3(BYTE* checksum, unsigned char *buffer, int size)                                                       
{
    printf("vtcm_SM3: Start\n");
    int ret = 0;
    sm3_context ctx; 
    sm3_starts(&ctx);
    sm3_update(&ctx, buffer, size);
    sm3_finish(&ctx, checksum);
    return ret;

}
int vtcm_SM3_1(BYTE* checksum, unsigned char* buffer,int size, unsigned char* buffer1,int size1)                                                       
{
    printf("vtcm_SM3: Start\n");
    int ret = 0;
    sm3_context ctx; 
    sm3_starts(&ctx);
    sm3_update(&ctx, buffer, size);
    sm3_update(&ctx,buffer1,size1);
    sm3_finish(&ctx, checksum);
    return ret;
}
int vtcm_SM3_2(BYTE* checksum, unsigned char* buffer1,int size1, unsigned char* buffer2,int size2,unsigned char* buffer3,int size3,unsigned char* buffer4,int size4)                                                       
{
    int ret = 0;
    sm3_context ctx; 
    sm3_starts(&ctx);
    sm3_update(&ctx, buffer1, size1);
    sm3_update(&ctx,buffer2,size2);
    sm3_update(&ctx,buffer3,size3);
    sm3_update(&ctx,buffer4,size4);
    sm3_finish(&ctx, checksum);
    return ret;
}
int vtcm_SM3_3(BYTE* checksum, unsigned char* buffer1,int size1, unsigned char* buffer2,int size2,unsigned char *buffer3,int size3)
{
    printf("vtcm_SM3: Start\n");
    int ret = 0;
    sm3_context ctx; 
    sm3_starts(&ctx);
    sm3_update(&ctx, buffer1, size1);
    sm3_update(&ctx,buffer2,size2);
    sm3_update(&ctx,buffer3,size3);
    sm3_finish(&ctx, checksum);
    return ret;
}
int vtcm_SM3_hmac(BYTE* checksum , unsigned char* key,int klen,unsigned char* hmac1,int size1, unsigned char* hmac2,int size2)                                                       
{
    printf("vtcm_SM3_HMAC: Start\n");
    int ret = 0;
    sm3_context ctx; 
    sm3_hmac_starts(&ctx,key,klen);
    sm3_hmac_update(&ctx, hmac1, size1);
    sm3_hmac_update(&ctx,hmac2,size2);
    sm3_hmac_finish(&ctx, checksum);
    return ret;
}
/*int print_error(char * str, int result)
  {
  printf("%s %s",str,tss_err_string(result));
  }*/

int vtcm_utils_init(void * sub_proc,void * para)
{
    INIT_LIST_HEAD(&sessions_list.list);
    sessions_list.record=NULL;

    INIT_LIST_HEAD(&sessions_list.list);
    sessions_list.record=NULL;
    return 0;
}
int TSS_gennonce(unsigned char *nonce){
    return RAND_bytes(nonce,TCM_HASH_SIZE);
}
int vtcm_utils_start(void * sub_proc,void * para)
{
    int ret;
    int retval;
    void * recv_msg;
    void * send_msg;
    void * context;
    void * sock;
    BYTE uuid[DIGEST_SIZE];
    int i;
    int type;
    int subtype;

    //	print_cubeaudit("begin proc vtcm_utils \n");

    //    ret=proc_vtcmutils_start(sub_proc,para);
    //    if(ret<0)
    //        return ret;

    while(1)
    {
        usleep(time_val.tv_usec);
        ret=ex_module_recvmsg(sub_proc,&recv_msg);
        if(ret<0)
            continue;
        if(recv_msg==NULL)
            continue;

        type=message_get_type(recv_msg);
        subtype=message_get_subtype(recv_msg);
        if((type==DTYPE_VTCM_UTILS) &&(subtype ==SUBTYPE_TCM_UTILS_INPUT))
        {
            ret=proc_vtcmutils_input(sub_proc,recv_msg);
        }
    }

    return 0;
};


int proc_vtcmutils_input(void * sub_proc,void * recv_msg)
{
    int ret = 0;

    struct tcm_utils_input * input_para;
    ret==message_get_record(recv_msg,&input_para,0);

    if(ret<0)
        return -EINVAL;

    if(input_para->param_num<1)
    {
        print_cubeerr("wrong command format! should be %s [cmd] [para] ...\n","main_proc");
        return -EINVAL;
    }
    if(strcmp(input_para->params,"createek")==0)
    {
        ret=proc_vtcmutils_createEKPair(sub_proc,input_para);
    }
    else if(strcmp(input_para->params,"extend")==0)
    {
        ret=proc_vtcmutils_Extend(sub_proc,input_para);
    }
    else if(strcmp(input_para->params,"pcrread")==0)
    {
        ret=proc_vtcmutils_PcrRead(sub_proc,input_para);
    }
    else if(strcmp(input_para->params,"readpubek")==0)
    {
        ret=proc_vtcmutils_readPubek(sub_proc,input_para);
    }
    else if(strcmp(input_para->params,"apcreate")==0)
    {
        ret=proc_vtcmutils_APCreate(sub_proc, input_para);
    }
    else if(strcmp(input_para->params,"physicalenable")==0)
    {
        ret=proc_vtcmutils_PhysicalEnable(sub_proc,input_para);
    }
    else if(strcmp(input_para->params,"physicaldisable")==0)
    {
        ret=proc_vtcmutils_PhysicalDisable(sub_proc,input_para);
    }
    else if(strcmp(input_para->params,"physicalsetdeactivated")==0)
    {
        ret=proc_vtcmutils_PhysicalSetDeactivated(sub_proc,input_para);
    }
    else if(strcmp(input_para->params,"takeownership")==0)
    {
        ret=proc_vtcmutils_takeownership(sub_proc,input_para);
    }
    else if(strcmp(input_para->params,"getcapability")==0)
    {
        ret=proc_vtcmutils_getcapability(sub_proc,input_para);
    }
    else if(strcmp(input_para->params,"getrandom")==0)
    {
        ret=proc_vtcmutils_getRandom(sub_proc,input_para);
    }
    else if(strcmp(input_para->params,"startup")==0)
    {
        ret=proc_vtcmutils_StartUp(sub_proc,input_para);
    }
    else if(strcmp(input_para->params,"sm3start")==0)
    {
        ret=proc_vtcmutils_SM3Start(sub_proc,input_para);
    }
    else if(strcmp(input_para->params,"sm3update")==0)
    {
        ret=proc_vtcmutils_SM3Update(sub_proc,input_para);
    }
    else if(strcmp(input_para->params,"sm3complete")==0)
    {
        ret=proc_vtcmutils_SM3Complete(sub_proc,input_para);
    }
    else if(strcmp(input_para->params,"createwrapkey")==0)
    {
        ret=proc_vtcmutils_CreateWrapKey(sub_proc,input_para);
    }
    else if(strcmp(input_para->params,"loadkey")==0)
    {
        ret=proc_vtcmutils_LoadKey(sub_proc,input_para);
    }
    else if(strcmp(input_para->params,"sign")==0)
    {
       // ret=proc_vtcmutils_Sign(sub_proc,input_para);
    }
    else if(strcmp(input_para->params,"wrapkey")==0)
    {
     //   ret=proc_vtcmutils_WrapKey(sub_proc,input_para);
    }
    else if(strcmp(input_para->params,"selftestfull")==0)
    {
          ret=proc_vtcmutils_SelfTestFull(sub_proc,input_para);
    }
    else if(strcmp(input_para->params,"sm3extend")==0)
    {
        ret=proc_vtcmutils_SM3CompleteExtend(sub_proc,input_para);
    }
    else if(strcmp(input_para->params,"apterminate")==0)
    {
        ret=proc_vtcmutils_APTerminate(sub_proc,input_para);
    }
    else if(strcmp(input_para->params,"pcrreset")==0)
    {
        ret=proc_vtcmutils_PcrReset(sub_proc,input_para);
    }
    else  if(strcmp(input_para->params,"continueselftest")==0)
    {
        ret=proc_vtcmutils_ContinueSelfTest(sub_proc,input_para);
    }
    else  if(strcmp(input_para->params,"gettestresult")==0)
    {
        ret=proc_vtcmutils_GetTestResult(sub_proc,input_para);
    }
    else if(strcmp(input_para->params,"forceclear")==0)
    {
        ret=proc_vtcmutils_ForceClear(sub_proc,input_para);
    }
    else if(strcmp(input_para->params,"disableforceclear")==0)
    {
        ret=proc_vtcmutils_DisableForceClear(sub_proc,input_para);
    }
    else if(strcmp(input_para->params,"ownerclear")==0)
    {
        ret=proc_vtcmutils_OwnerClear(sub_proc,input_para);
    }
    else if(strcmp(input_para->params,"disableownerclear")==0)
    {
        ret=proc_vtcmutils_DisableOwnerClear(sub_proc,input_para);
    }
    else if(strcmp(input_para->params,"flushspecific")==0)
    {
        ret=proc_vtcmutils_FlushSpecific(sub_proc,input_para);
    }
    else if(strcmp(input_para->params,"nvdefinespace")==0)
    {
        ret=proc_vtcmutils_NV_DefineSpace(sub_proc,input_para);
    }
    else if(strcmp(input_para->params,"nvwritevalue")==0)
    {
        ret=proc_vtcmutils_NV_WriteValue(sub_proc,input_para);
    }
    else if(strcmp(input_para->params,"readvalue")==0)
    {
        ret=proc_vtcmutils_NV_ReadValue(sub_proc,input_para);
    }
    else if(strcmp(input_para->params,"sm4encrypt")==0)
    {
        ret=proc_vtcmutils_SM4Encrypt(sub_proc,input_para);
    }
    else if(strcmp(input_para->params,"sm4decrypt")==0)
    {
        ret=proc_vtcmutils_SM4Decrypt(sub_proc,input_para);
    }
    else if(strcmp(input_para->params,"sm2encrypt")==0)
    {
        ret=proc_vtcmutils_SM2Encrypt(sub_proc,input_para);
    }
    else if(strcmp(input_para->params,"sm2decrypt")==0)
    {
        ret=proc_vtcmutils_SM2Decrypt(sub_proc,input_para);
    }
    ///    if(strcmp(input_para->params,"ownerreadinternalpub")==0)
    //      {
    //        ret=proc_vtcmutils_OwnerReadInternalPub(sub_proc,input_para);
    //      }
    //     if(strcmp(input->params,"certifykey")==0)
    //      {
    //        ret=proc_vtcmutils_CertifyKey(sub_proc,input_para);   
    //      }
    else if(strcmp(input_para->params,"createsm2key")==0)
    {
        ret=proc_vtcmutils_ExCreateSm2Key(sub_proc,input_para);
    }
    else if(strcmp(input_para->params,"loadcakey")==0)
    {
        ret=proc_vtcmutils_ExLoadCAKey(sub_proc,input_para);
    }
    else if(strcmp(input_para->params,"makeidentity")==0)
    {
        ret=proc_vtcmutils_MakeIdentity(sub_proc,input_para);
    }
    else if(strcmp(input_para->params,"quote")==0)
    {
        ret=proc_vtcmutils_Quote(sub_proc,input_para);
    }
    else if(strcmp(input_para->params,"seal")==0)
    {
        ret=proc_vtcmutils_Seal(sub_proc,input_para);
    }
    else if(strcmp(input_para->params,"Unseal")==0)
    {
       // ret=proc_vtcmutils_Unseal(sub_proc,input_para);
    }
    else if(strcmp(input_para->params,"Sign")==0)
    {
      //  ret=proc_vtcmutils_Sign(sub_proc,input_para);
    }
    else if(strcmp(input_para->params,"changeauth")==0)
    {
       // ret=proc_vtcmutils_ChangeAuth(sub_proc,input_para);
    }
    else
    {
        printf("function error\n");
    }
    return ret;
}
/*int proc_vtcmutils_ChangeAuth(void * sub_proc, void * para){
    TCM_KEY *keyOut;
    unsigned char *encData=NULL;
    int i=1;
    int outlen;
    int ret=0;
    char *keyfile=NULL;
    void * vtcm_template;
    unsigned char hashout[TCM_HASH_SIZE];
    unsigned char hmacout[TCM_HASH_SIZE];
    struct tcm_in_ChangeAuth *vtcm_input;
    struct tcm_out_ChangeAuth *vtcm_output;
    TCM_SESSION_DATA * authdata;
    unsigned char encryptauthdata[TCM_HASH_SIZE];
    char * newauthdata = "ddd";
    sm3(newauthdata,strlen(newauthdata),encryptauthdata);
    vtcm_input = Talloc0(sizeof(*vtcm_input));
    if(vtcm_input==NULL)
        return -ENOMEM;
    vtcm_output = Talloc0(sizeof(*vtcm_output));
    if(vtcm_output==NULL)
        return -ENOMEM;
    vtcm_input->tag = htons(TCM_TAG_RQU_AUTH2_COMMAND);
    vtcm_input->ordinal = SUBTYPE_CHANGEAUTH_IN;
    vtcm_input->protocolID = 0x08;
    Memcpy(vtcm_input->newAuth,encryptauthdata,TCM_HASH_SIZE);
    vtcm_input->entityType = 0x05;
    vtcm_input->encDataSize=0;
    vtcm_input->encData = NULL;
    struct tcm_utils_input * input_para=para;
    char *curr_para;
    while (i<input_para->param_num) {
        curr_para=input_para->params+i*DIGEST_SIZE;
        if (!strcmp("-ifh",curr_para)) {
            i++;
            curr_para=input_para->params+i*DIGEST_SIZE;
            if (i < input_para->param_num)
            {
                sscanf(curr_para,"%x",&vtcm_input->parentHandle); 
            }else{
                printf("Missing parameter for -ikh.\n");
                return -1;
            } 
        }else if (!strcmp("-idh",curr_para)) {
            i++;
            curr_para=input_para->params+i*DIGEST_SIZE;
            if (i < input_para->param_num)
            {
                sscanf(curr_para,"%x",&vtcm_input->DecryptAuthHandle); 
            }else{
                printf("Missing parameter for -idh.\n");
                return -1;
            } 
        }else if(!strcmp(curr_para,"-rf")){
            i++;
            curr_para=input_para->params+i*DIGEST_SIZE;
            if(i<input_para->param_num){
               keyfile=curr_para;
            }
        }
        i++;
    }
    int fd;
    int datasize;
    authdata=Find_AuthSession(0x01,vtcm_input->DecryptAuthHandle);
    fd=open(keyfile,O_RDONLY);
    if(fd<0)
         return -EIO;
    ret=read(fd,Buf,DIGEST_SIZE*32+1);
    if(ret<0)
        return -EIO;
    if(ret>DIGEST_SIZE*32)
    {
        printf("key file too large!\n");
        return -EINVAL;     
    }
    datasize=ret;
    vtcm_input->DecryptDataSize =datasize ; 
    vtcm_input->paramSize = datasize+54;
     //compute DecryptAuthVerfication
    vtcm_input->DecryptData = Talloc0(vtcm_input->DecryptDataSize);
    if(vtcm_input->DecryptData==NULL)
    	return -EINVAL;
    print_bin_data(Buf,datasize,8);
    Memcpy(vtcm_input->DecryptData,Buf,vtcm_input->DecryptDataSize);
*/
/*
    int ordinal = htonl(vtcm_input->ordinal);
    vtcm_SM3_3(hashout,&ordinal,4,&datasize,4,Buf,datasize);
    authdata=Find_AuthSession(0x01,vtcm_input->DecryptAuthHandle);
    int serial = htonl(authdata->SERIAL);
    vtcm_SM3_hmac(hmacout,authdata->sharedSecret,32,hashout,32,&serial,4);
    Memcpy(vtcm_input->DecryptAuthVerfication,hmacout,TCM_HASH_SIZE); 

    printf("Begin input for SM2Decrypt\n");
    vtcm_template=memdb_get_template(DTYPE_VTCM_IN,SUBTYPE_SM2DECRYPT_IN);
    ret =  struct_2_blob(vtcm_input,Buf,vtcm_template);
    if(ret<0)
        return ret;
*/
/*    ret=vtcm_Compute_AuthCode(vtcm_input,authdata,NULL,DTYPE_VTCM_IN,SUBTYPE_SM2DECRYPT_IN,Buf);
    print_bin_data(Buf,ret,8);                                                                            
    ret = vtcmutils_transmit(vtcm_input->paramSize,Buf,&outlen,Buf);
    if(ret<0)
        return ret;
    print_bin_data(Buf,outlen,8);

}*/
int proc_vtcmutils_NV_DefineSpace(void * sub_proc, void * para){

   
    int i=1;
    int j=0;
    int outlen;
    int ret = 0;
    char * message = NULL;
    struct tcm_in_NV_DefineSpace * vtcm_input;
    struct tcm_out_NV_DefineSpace * vtcm_output;
    void * vtcm_template;
    unsigned char msghash[32];
    TCM_SESSION_DATA * authdata;
    BYTE nvauth[TCM_HASH_SIZE];
  
    // nv input parameter
    int index = -1;
    int size=0;
    char * passwd=NULL;
    unsigned int permission=0;		
    TCM_AUTHHANDLE nvHandle=0;
    int cmd_scene=0;	  // 1 means physical auth, 2 means one auth
    
    printf("Begin send message for NVDefineSpace:\n");
    vtcm_input = Talloc0(sizeof(*vtcm_input));
    if(vtcm_input==NULL)
        return -ENOMEM;
    vtcm_output = Talloc0(sizeof(*vtcm_output));
    if(vtcm_output==NULL)
        return -ENOMEM;

    struct tcm_utils_input * input_para=para;
    char * index_para;
    char * value_para;

	if((input_para->param_num>0)&&
		(input_para->param_num%2==1))
	{
		for(i=1;i<input_para->param_num;i+=2)
		{
        		index_para=input_para->params+i*DIGEST_SIZE;
        		value_para=index_para+DIGEST_SIZE;
			if(!Strcmp("-in",index_para))
			{
        			index=Atoi(value_para,DIGEST_SIZE);
			}	
			else if(!Strcmp("-sz",index_para))
			{
				size=Atoi(value_para,DIGEST_SIZE);
			}
			else if(!Strcmp("-pwd",index_para))
			{
				passwd=value_para;
			}
			else if(!Strcmp("-per",index_para))
			{
				sscanf(value_para,"%x",&permission);
			}
			else if(!Strcmp("-ih",index_para))
			{
				sscanf(value_para,"%x",&nvHandle);
			}
			else
			{
				printf("Error cmd format! should be %s -in index -sz size -pwd passwd -per permission"
					"[-ih nvHandle]",input_para->params);
				return -EINVAL;
			}
		}
	}	


    if(nvHandle==0)
   {
    	vtcm_input->tag = htons(TCM_TAG_RQU_COMMAND);
   }
   else
   {
	vtcm_input->tag=htons(TCM_TAG_RQU_AUTH1_COMMAND);	
    	authdata = Find_AuthSession(TCM_ET_NV,nvHandle);
   	 if(authdata==NULL)
    	{
       		 printf("can't find session for nv_definespace!\n");
        	return -EINVAL;
    	}	
   } 	

    vtcm_input->ordinal=SUBTYPE_NV_DEFINESPACE_IN;

    // fill vtcm_input 's parameters	
    vtcm_input->pubInfo.tag= htons(TCM_TAG_NV_DATA_PUBLIC);
    vtcm_input->pubInfo.nvIndex=index;

    vtcm_Init_PcrInfo(&vtcm_input->pubInfo.pcrInfoRead);	
    vtcm_Init_PcrInfo(&vtcm_input->pubInfo.pcrInfoWrite);	

    vtcm_input->pubInfo.permission.tag=htons(TCM_TAG_NV_ATTRIBUTES);
    vtcm_input->pubInfo.permission.attributes=permission;
    vtcm_input->pubInfo.bReadSTClear=TRUE;
    vtcm_input->pubInfo.bWriteSTClear=FALSE;
    vtcm_input->pubInfo.bWriteDefine=FALSE;
    vtcm_input->pubInfo.dataSize=size;

    memset(vtcm_input->encAuth,0,TCM_HASH_SIZE);
    vtcm_input->authHandle=nvHandle;
    memset(vtcm_input->ownerAuth,0,TCM_HASH_SIZE);
    vtcm_template=memdb_get_template(DTYPE_VTCM_IN,SUBTYPE_NV_DEFINESPACE_IN);
    if(vtcm_template==NULL)
        return -EINVAL;
    int offset=0;

    if(nvHandle==0)
    {
   	 offset = struct_2_blob(vtcm_input,Buf,vtcm_template);
    	 if(offset<0)
        	return offset;
         vtcm_input->paramSize=offset-36;
   	 ret = struct_2_blob(vtcm_input,Buf,vtcm_template);
    	 if(ret<0)
        	return ret;
    }
    else
    {	
   	 offset = struct_2_blob(vtcm_input,Buf,vtcm_template);
    	 if(offset<0)
        	return offset;
         vtcm_input->paramSize=offset;
         ret =  struct_2_blob(vtcm_input,Buf,vtcm_template);
         if(ret<0)
               return ret;
    }
    print_bin_data(Buf,ret,8);
    ret = vtcmutils_transmit(vtcm_input->paramSize,Buf,&outlen,Buf);
    if(ret<0)
        return ret;
    vtcm_template=memdb_get_template(DTYPE_VTCM_OUT,SUBTYPE_NV_DEFINESPACE_OUT);
    if(vtcm_template==NULL)
        return -EINVAL;
    print_bin_data(Buf,outlen,8);

    sprintf(Buf,"%d \n",vtcm_output->returnCode);

    void * send_msg =vtcm_auto_build_outputmsg(Buf,NULL);

    if(send_msg==NULL)
	return -EINVAL;

    ex_module_sendmsg(sub_proc,send_msg);		
    return ret;
}

int proc_vtcmutils_NV_WriteValue(void * sub_proc, void * para){
    int i=1;
    int outlen;
    int ret = 0;
    struct tcm_in_NV_WriteValue * vtcm_input;
    struct tcm_out_NV_WriteValue * vtcm_output;
    void * vtcm_template;
    unsigned char msghash[32];
    TCM_SESSION_DATA * authdata;
    BYTE nvauth[TCM_HASH_SIZE];

    // nv input parameter
    int index = -1;
    int size=0;
    char * passwd=NULL;
    int offset=0;
    char * writemsg=NULL;
    TCM_AUTHHANDLE nvHandle=0;

    printf("Begin send message for NVWriteValue:\n");
    vtcm_input = Talloc0(sizeof(*vtcm_input));
    if(vtcm_input==NULL)
        return -ENOMEM;
    vtcm_output = Talloc0(sizeof(*vtcm_output));
    if(vtcm_output==NULL)
        return -ENOMEM;

    struct tcm_utils_input * input_para=para;
    char * index_para;
    char * value_para;

	if((input_para->param_num>0)&&
		(input_para->param_num%2==1))
	{
		for(i=1;i<input_para->param_num;i+=2)
		{
        		index_para=input_para->params+i*DIGEST_SIZE;
        		value_para=index_para+DIGEST_SIZE;
			if(!Strcmp("-in",index_para))
			{
        			index=Atoi(value_para,DIGEST_SIZE);
			}	
			else if(!Strcmp("-sz",index_para))
			{
				size=Atoi(value_para,DIGEST_SIZE);
			}
			else if(!Strcmp("-pwd",index_para))
			{
				passwd=value_para;
			}
			else if(!Strcmp("-off",index_para))
			{
				offset=Atoi(value_para,DIGEST_SIZE);
			}
			else if(!Strcmp("-ic",index_para))
			{
				writemsg=value_para;
			}
			else if(!Strcmp("-ih",index_para))
			{
				sscanf(value_para,"%x",&nvHandle);
			}
			else
			{
				printf("Error cmd format! should be %s -in index -sz size -pwd passwd -off offset"
					"-ic write_message [-ih nvHandle]",input_para->params);
				return -EINVAL;
			}
		}
	}	

    vtcm_input->ordinal=SUBTYPE_NV_WRITEVALUE_IN;
    if(nvHandle==0)
   {
    	vtcm_input->tag = htons(TCM_TAG_RQU_COMMAND);
   }
   else
   {
	vtcm_input->tag=htons(TCM_TAG_RQU_AUTH1_COMMAND);	
    	authdata = Find_AuthSession(TCM_ET_OWNER,nvHandle);
   	 if(authdata==NULL)
    	{
       		 printf("can't find session for nv_writevalue!\n");
        	return -EINVAL;
    	}	
   } 	

    vtcm_input->offset=offset;
    	
   if(writemsg==NULL)
    {
	  printf("No write data!\n");
	  return -EINVAL;
    }
    if(size==0)
    {
	vtcm_input->dataSize=Strnlen(writemsg,DIGEST_SIZE);
    }
    else
    {
	vtcm_input->dataSize=size;
    }    
    vtcm_input->data=Talloc0(vtcm_input->dataSize);
    if(vtcm_input->data==NULL)
	return -ENOMEM;
    Strncpy(vtcm_input->data,writemsg,vtcm_input->dataSize);

    vtcm_input->authHandle=nvHandle;
    memset(vtcm_input->ownerAuth,0,TCM_HASH_SIZE);
    vtcm_template=memdb_get_template(DTYPE_VTCM_IN,SUBTYPE_NV_WRITEVALUE_IN);
    if(vtcm_template==NULL)
        return -EINVAL;
    if(nvHandle==0)
    {
   	 offset = struct_2_blob(vtcm_input,Buf,vtcm_template);
    	 if(offset<0)
        	return offset;
         vtcm_input->paramSize=offset-36;
   	 ret = struct_2_blob(vtcm_input,Buf,vtcm_template);
    	 if(ret<0)
        	return ret;
    }
    else
    {	
   	 offset = struct_2_blob(vtcm_input,Buf,vtcm_template);
    	 if(offset<0)
        	return offset;
         vtcm_input->paramSize=offset;
         ret =  struct_2_blob(vtcm_input,Buf,vtcm_template);
         if(ret<0)
               return ret;
    }
    print_bin_data(Buf,ret,8);
    ret = vtcmutils_transmit(vtcm_input->paramSize,Buf,&outlen,Buf);
    if(ret<0)
        return ret;
    print_bin_data(Buf,outlen,8);
    sprintf(Buf,"%d\n ",vtcm_output->returnCode);

    void * send_msg =vtcm_auto_build_outputmsg(Buf,NULL);

    if(send_msg==NULL)
  	return -EINVAL;

    ex_module_sendmsg(sub_proc,send_msg);		
    return ret;
}

int proc_vtcmutils_NV_ReadValue(void * sub_proc, void * para){
    int i=1;
    int outlen;
    int ret = 0;
    char * message = NULL;
    struct tcm_in_NV_WriteValue * vtcm_input;
    struct tcm_out_NV_WriteValue * vtcm_output;
    void * vtcm_template;
    unsigned char msghash[32];
    printf("Begin send message for NVWriteValue:\n");
    vtcm_input = Talloc0(sizeof(*vtcm_input));
    if(vtcm_input==NULL)
        return -ENOMEM;
    vtcm_output = Talloc0(sizeof(*vtcm_output));
    if(vtcm_output==NULL)
        return -ENOMEM;
    struct tcm_utils_input * input_para=para;
    char * curr_para;
    while(i<input_para->param_num){
        curr_para=input_para->params+i*DIGEST_SIZE;
        if(!strcmp("-in",curr_para)){
            i++;
            curr_para=input_para->params+i*DIGEST_SIZE;
            if(i<input_para->param_num){
                sscanf(curr_para,"%d",&vtcm_input->nvIndex);
            }
            else{
                printf("Missing parameter for -in.\n");
                return -1;
            }	
        }
 
        i++;
    }
    vtcm_input->tag=htons(TCM_TAG_RQU_COMMAND);
    vtcm_input->ordinal=SUBTYPE_NV_WRITEVALUE_IN;
    vtcm_input->offset=0;
    memset(vtcm_input->data,0,TCM_HASH_SIZE);
    vtcm_input->dataSize=0x20;
    vtcm_template=memdb_get_template(DTYPE_VTCM_IN,SUBTYPE_NV_WRITEVALUE_IN);
    if(vtcm_template==NULL)
        return -EINVAL;
    int offset=0;
    offset = struct_2_blob(vtcm_input,Buf,vtcm_template);
    if(offset<0)
        return offset;
    vtcm_input->paramSize=offset;
    ret =  struct_2_blob(vtcm_input,Buf,vtcm_template);
    if(ret<0)
        return ret;
    print_bin_data(Buf,ret,8);
    ret = vtcmutils_transmit(vtcm_input->paramSize,Buf,&outlen,Buf);
    if(ret<0)
        return ret;
    print_bin_data(Buf,outlen,8);
    return ret;
}
/*int proc_vtcmutils_UnSeal(void * sub_proc, void * para){
    int i=1;
    int outlen;
    int ret=0;
    void * vtcm_template;
    char *seal="sealauth";
    unsigned char sealdata[TCM_HASH_SIZE];
    unsigned char hashout[TCM_HASH_SIZE];
    unsigned char hmacout[TCM_HASH_SIZE];
    struct tcm_in_UnSeal *vtcm_input;
    struct tcm_out_UnSeal *vtcm_output;
    TCM_SESSION_DATA * authdata;
    vtcm_input = Talloc0(sizeof(*vtcm_input));
    if(vtcm_input==NULL)
        return -ENOMEM;
    vtcm_output = Talloc0(sizeof(*vtcm_output));
    if(vtcm_output==NULL)
        return -ENOMEM;
    vtcm_input->tag = htons(TCM_TAG_RQU_AUTH2_COMMAND);
    vtcm_input->ordinal = SUBTYPE_UNSEAL_IN;
    struct tcm_utils_input * input_para=para;
    char *curr_para;
    while (i<input_para->param_num) {
        curr_para=input_para->params+i*DIGEST_SIZE;
        if (!strcmp("-ikh",curr_para)) {
            i++;
            curr_para=input_para->params+i*DIGEST_SIZE;
            if (i < input_para->param_num)
            {
                sscanf(curr_para,"%x",&vtcm_input->keyHandle); 
            }else{
                printf("Missing parameter for -ikh.\n");
                return -1;
            } 
        }else if (!strcmp("-idh",curr_para)) {
            i++;
            curr_para=input_para->params+i*DIGEST_SIZE;
            if (i < input_para->param_num)
            {
                sscanf(curr_para,"%x",&vtcm_input->authHandle); 
            }else{
                printf("Missing parameter for -idh.\n");
                return -1;
            } 
        }
        i++;
    }
    sm3(seal,strlen(seal),sealdata);
    Memcpy(vtcm_input->InData,sealdata,TCM_HASH_SIZE) ; 
    memset(vtcm_input->encAuth,0,TCM_HASH_SIZE);
    vtcm_input->InDataSize=32;
    vtcm_input->pcrInfo=NULL;
    vtcm_input->pcrInfoSize=0;
    // compute authcode
    int ordinal = htonl(vtcm_input->ordinal);
   // int signdatalength=htonl(vtcm_input->areaToSignSize);
   // vtcm_SM3_3(hashout,&ordinal,4,&signdatalength,4,signeddata,32);
  //  authdata=Find_AuthSession(0x01,vtcm_input->authHandle);
 //   int serial = htonl(authdata->SERIAL);
 //   vtcm_SM3_hmac(hmacout,authdata->sharedSecret,32,hashout,32,&serial,4);
  //  Memcpy(vtcm_input->authCode,hmacout,TCM_HASH_SIZE); 

    printf("Begin input for Seal\n");
    vtcm_template=memdb_get_template(DTYPE_VTCM_IN,SUBTYPE_SEAL_IN);
    ret =  struct_2_blob(vtcm_input,Buf,vtcm_template);
    if(ret<0)
        return ret;
    print_bin_data(Buf,ret,8);                                                                            
    ret = vtcmutils_transmit(vtcm_input->paramSize,Buf,&outlen,Buf);
    if(ret<0)
        return ret;
    print_bin_data(Buf,outlen,8);

}*/
int proc_vtcmutils_Seal(void * sub_proc, void * para){
    int i=1;
    int outlen;
    int ret=0;
    void * vtcm_template;
    char *seal="sealauth";
    unsigned char sealdata[TCM_HASH_SIZE];
    unsigned char hashout[TCM_HASH_SIZE];
    unsigned char hmacout[TCM_HASH_SIZE];
    struct tcm_in_Seal *vtcm_input;
    struct tcm_out_Seal *vtcm_output;
    TCM_SESSION_DATA * authdata;
    vtcm_input = Talloc0(sizeof(*vtcm_input));
    if(vtcm_input==NULL)
        return -ENOMEM;
    vtcm_output = Talloc0(sizeof(*vtcm_output));
    if(vtcm_output==NULL)
        return -ENOMEM;
    vtcm_input->tag = htons(TCM_TAG_RQU_AUTH1_COMMAND);
    vtcm_input->ordinal = SUBTYPE_SEAL_IN;
    struct tcm_utils_input * input_para=para;
    char *curr_para;
    while (i<input_para->param_num) {
        curr_para=input_para->params+i*DIGEST_SIZE;
        if (!strcmp("-ikh",curr_para)) {
            i++;
            curr_para=input_para->params+i*DIGEST_SIZE;
            if (i < input_para->param_num)
            {
                sscanf(curr_para,"%x",&vtcm_input->keyHandle); 
            }else{
                printf("Missing parameter for -ikh.\n");
                return -1;
            } 
        }else if (!strcmp("-idh",curr_para)) {
            i++;
            curr_para=input_para->params+i*DIGEST_SIZE;
            if (i < input_para->param_num)
            {
                sscanf(curr_para,"%x",&vtcm_input->authHandle); 
            }else{
                printf("Missing parameter for -idh.\n");
                return -1;
            } 
        }
        i++;
    }
    sm3(seal,strlen(seal),sealdata);
    Memcpy(vtcm_input->InData,sealdata,TCM_HASH_SIZE) ; 
    memset(vtcm_input->encAuth,0,TCM_HASH_SIZE);
    vtcm_input->InDataSize=32;
    vtcm_input->pcrInfo=NULL;
    vtcm_input->pcrInfoSize=0;
    // compute authcode
    int ordinal = htonl(vtcm_input->ordinal);
   // int signdatalength=htonl(vtcm_input->areaToSignSize);
   // vtcm_SM3_3(hashout,&ordinal,4,&signdatalength,4,signeddata,32);
  //  authdata=Find_AuthSession(0x01,vtcm_input->authHandle);
 //   int serial = htonl(authdata->SERIAL);
 //   vtcm_SM3_hmac(hmacout,authdata->sharedSecret,32,hashout,32,&serial,4);
  //  Memcpy(vtcm_input->authCode,hmacout,TCM_HASH_SIZE); 

    printf("Begin input for Seal\n");
    vtcm_template=memdb_get_template(DTYPE_VTCM_IN,SUBTYPE_SEAL_IN);
    ret =  struct_2_blob(vtcm_input,Buf,vtcm_template);
    if(ret<0)
        return ret;
    print_bin_data(Buf,ret,8);                                                                            
    ret = vtcmutils_transmit(vtcm_input->paramSize,Buf,&outlen,Buf);
    if(ret<0)
        return ret;
    print_bin_data(Buf,outlen,8);

}
int proc_vtcmutils_Sign(void * sub_proc, void * para){
    int i=1;
    int outlen;
    int ret=0;
    void * vtcm_template;
    char *sign="signdata";
    unsigned char signeddata[TCM_HASH_SIZE];
    unsigned char hashout[TCM_HASH_SIZE];
    unsigned char hmacout[TCM_HASH_SIZE];
    struct tcm_in_Sign *vtcm_input;
    struct tcm_out_Sign *vtcm_output;
    TCM_SESSION_DATA * authdata;
    vtcm_input = Talloc0(sizeof(*vtcm_input));
    if(vtcm_input==NULL)
        return -ENOMEM;
    vtcm_output = Talloc0(sizeof(*vtcm_output));
    if(vtcm_output==NULL)
        return -ENOMEM;
    vtcm_input->tag = htons(TCM_TAG_RQU_AUTH1_COMMAND);
    vtcm_input->ordinal = SUBTYPE_SIGN_IN;
    struct tcm_utils_input * input_para=para;
    char *curr_para;
    while (i<input_para->param_num) {
        curr_para=input_para->params+i*DIGEST_SIZE;
        if (!strcmp("-ikh",curr_para)) {
            i++;
            curr_para=input_para->params+i*DIGEST_SIZE;
            if (i < input_para->param_num)
            {
                sscanf(curr_para,"%x",&vtcm_input->keyHandle); 
            }else{
                printf("Missing parameter for -ikh.\n");
                return -1;
            } 
        }else if (!strcmp("-idh",curr_para)) {
            i++;
            curr_para=input_para->params+i*DIGEST_SIZE;
            if (i < input_para->param_num)
            {
                sscanf(curr_para,"%x",&vtcm_input->authHandle); 
            }else{
                printf("Missing parameter for -idh.\n");
                return -1;
            } 
        }
        i++;
    }
    sm3(sign,strlen(sign),signeddata);
    Memcpy(vtcm_input->areaToSign,signeddata,TCM_HASH_SIZE) ; 
    vtcm_input->areaToSignSize=0x20;
    // compute authcode
    int ordinal = htonl(vtcm_input->ordinal);
    int signdatalength=htonl(vtcm_input->areaToSignSize);
    vtcm_SM3_3(hashout,&ordinal,4,&signdatalength,4,signeddata,32);
    authdata=Find_AuthSession(0x01,vtcm_input->authHandle);
    int serial = htonl(authdata->SERIAL);
    vtcm_SM3_hmac(hmacout,authdata->sharedSecret,32,hashout,32,&serial,4);
    Memcpy(vtcm_input->privAuth,hmacout,TCM_HASH_SIZE); 

    printf("Begin input for Sign\n");
    vtcm_template=memdb_get_template(DTYPE_VTCM_IN,SUBTYPE_SIGN_IN);
    ret =  struct_2_blob(vtcm_input,Buf,vtcm_template);
    if(ret<0)
        return ret;
    print_bin_data(Buf,ret,8);                                                                            
    ret = vtcmutils_transmit(vtcm_input->paramSize,Buf,&outlen,Buf);
    if(ret<0)
        return ret;
    print_bin_data(Buf,outlen,8);

}

int proc_vtcmutils_SM2Encrypt(void * sub_proc, void * para){
    TCM_KEY *keyOut;
    unsigned char *encData=NULL;
    int i=1;
    int ret=0;
    char *keyfile=NULL;
    char *writefile=NULL;
    void * vtcm_template;
    char *encryptFile="helloworld";
    struct tcm_utils_input * input_para=para;
    char *curr_para;
    while (i<input_para->param_num) {
        curr_para=input_para->params+i*DIGEST_SIZE;
        if (!strcmp("-rf",curr_para)) {
            i++;
            curr_para=input_para->params+i*DIGEST_SIZE;
            if (i < input_para->param_num)
            {
                keyfile=curr_para;
            }else{
                printf("Missing parameter for -if.\n");
                return -1;
            } 
        }else if(!strcmp(curr_para,"-wf")){
            i++;
            curr_para=input_para->params+i*DIGEST_SIZE;
            if(i<input_para->param_num){
               writefile=curr_para;
            }
        }
        i++;
    }
    encData=(BYTE*)malloc(sizeof(BYTE)*512);
    int length=512;
    BYTE * keyFile=(BYTE*)malloc(sizeof(BYTE)*keyLength);
    int fd;
    int datasize;
    fd=open(keyfile,O_RDONLY);
    if(fd<0)
         return -EIO;
    ret=read(fd,Buf,DIGEST_SIZE*32+1);
    if(ret<0)
        return -EIO;
    if(ret>DIGEST_SIZE*32)
    {
        printf("key file too large!\n");
        return -EINVAL;     
    }
    close(fd);
    datasize=ret;
   // proc_vtcmutils_ReadFile(keyLength,keyFile);
    vtcm_template=memdb_get_template(DTYPE_VTCM_IN_KEY,SUBTYPE_TCM_BIN_KEY);
    if(vtcm_template==NULL)
        return -EINVAL;

    keyOut=Talloc0(sizeof(*keyOut));
    if(keyOut==NULL)
	return -ENOMEM;

    ret=blob_2_struct(Buf,keyOut,vtcm_template);
    if(ret<0||ret>datasize){
        printf("read key file error!\n");
            return -EINVAL;
    }
    int returnlen= GM_SM2Encrypt(encData, &length,encryptFile ,strlen(encryptFile),keyOut->pubKey.key, keyOut->pubKey.keyLength);
    if(returnlen!=0){
        printf("SM2Encrypt is fail\n");
    }
    printf("%d\n",strlen(encryptFile));
    printf("%d\n",returnlen);
    printf("gongyao is :\n");
    print_bin_data(keyOut->pubKey.key,keyOut->pubKey.keyLength,8);
    printf("\n");
    fd=open(writefile,O_CREAT|O_TRUNC|O_WRONLY,0666);
    if(fd<0){
        printf("file open error!\n");
        return -EIO;
    }
    print_bin_data(encData,length,8);
    write(fd,encData,length);
    close(fd);
    sprintf(Buf,"%d \n",0);

   void * send_msg =vtcm_auto_build_outputmsg(Buf,NULL);

   if(send_msg==NULL)
	return -EINVAL;

   ex_module_sendmsg(sub_proc,send_msg);		
    return ret;
}
int proc_vtcmutils_SM2Decrypt(void * sub_proc, void * para){
    TCM_KEY *keyOut;
    unsigned char *encData=NULL;
    int i=1;
    int outlen;
    int ret=0;
    char *keyfile=NULL;
    void * vtcm_template;
    unsigned char hashout[TCM_HASH_SIZE];
    unsigned char hmacout[TCM_HASH_SIZE];
    struct tcm_in_Sm2Decrypt *vtcm_input;
    struct tcm_out_Sm2Decrypt *vtcm_output;
    TCM_SESSION_DATA * authdata;
    vtcm_input = Talloc0(sizeof(*vtcm_input));
    if(vtcm_input==NULL)
        return -ENOMEM;
    vtcm_output = Talloc0(sizeof(*vtcm_output));
    if(vtcm_output==NULL)
        return -ENOMEM;
    vtcm_input->tag = htons(TCM_TAG_RQU_AUTH1_COMMAND);
    vtcm_input->ordinal = SUBTYPE_SM2DECRYPT_IN;
    struct tcm_utils_input * input_para=para;
    char *curr_para;
    while (i<input_para->param_num) {
        curr_para=input_para->params+i*DIGEST_SIZE;
        if (!strcmp("-ikh",curr_para)) {
            i++;
            curr_para=input_para->params+i*DIGEST_SIZE;
            if (i < input_para->param_num)
            {
                sscanf(curr_para,"%x",&vtcm_input->keyHandle); 
            }else{
                printf("Missing parameter for -ikh.\n");
                return -1;
            } 
        }else if (!strcmp("-idh",curr_para)) {
            i++;
            curr_para=input_para->params+i*DIGEST_SIZE;
            if (i < input_para->param_num)
            {
                sscanf(curr_para,"%x",&vtcm_input->DecryptAuthHandle); 
            }else{
                printf("Missing parameter for -idh.\n");
                return -1;
            } 
        }else if(!strcmp(curr_para,"-rf")){
            i++;
            curr_para=input_para->params+i*DIGEST_SIZE;
            if(i<input_para->param_num){
               keyfile=curr_para;
            }
        }
        i++;
    }
    int fd;
    int datasize;
    authdata=Find_AuthSession(0x01,vtcm_input->DecryptAuthHandle);
    fd=open(keyfile,O_RDONLY);
    if(fd<0)
         return -EIO;
    ret=read(fd,Buf,DIGEST_SIZE*32+1);
    if(ret<0)
        return -EIO;
    if(ret>DIGEST_SIZE*32)
    {
        printf("key file too large!\n");
        return -EINVAL;     
    }
    datasize=ret;
    vtcm_input->DecryptDataSize =datasize ; 
    vtcm_input->paramSize = datasize+54;
     //compute DecryptAuthVerfication
    vtcm_input->DecryptData = Talloc0(vtcm_input->DecryptDataSize);
    if(vtcm_input->DecryptData==NULL)
    	return -EINVAL;
    print_bin_data(Buf,datasize,8);
    Memcpy(vtcm_input->DecryptData,Buf,vtcm_input->DecryptDataSize);
    ret=vtcm_Compute_AuthCode(vtcm_input,authdata,NULL,DTYPE_VTCM_IN,SUBTYPE_SM2DECRYPT_IN,Buf);
    print_bin_data(Buf,ret,8);                                                                            
    ret = vtcmutils_transmit(vtcm_input->paramSize,Buf,&outlen,Buf);
    if(ret<0)
        return ret;
    print_bin_data(Buf,outlen,8);
    printf("Decrypted data is:\n");
    BYTE dedata[10];
    for(i=0;i<10;i++){
        dedata[i]=Buf[14+i];
        printf("%c",dedata[i]);
        // printf("%c",(char)dedata[i]);
    }
    printf("\n");
}
int proc_vtcmutils_SM4Encrypt(void * sub_proc, void * para){
    int outlen;
    int i=1;
    int ret=0;
    void *vtcm_template;
    struct tcm_in_Sm4Encrypt *vtcm_input;
    struct tcm_out_Sm4Encrypt *vtcm_output;
    unsigned char nonce[TCM_HASH_SIZE];
    unsigned char cbcModle[16];
    unsigned char hashout[TCM_HASH_SIZE];
    unsigned char hmacout[TCM_HASH_SIZE];
    char *en4data = "helloworldsm4";
    char *keyfile = NULL;
    vtcm_input = Talloc0(sizeof(*vtcm_input));
    if(vtcm_input==NULL)
        return -ENOMEM;
    vtcm_output = Talloc0(sizeof(*vtcm_output));
    if(vtcm_output==NULL)
        return -ENOMEM;
    // insert data that will be encrypt and handle
    struct tcm_utils_input * input_para=para;
    char *curr_para;
    while (i<input_para->param_num) {
        curr_para=input_para->params+i*DIGEST_SIZE;
        if (!strcmp("-ikh",curr_para)) {
            i++;
            curr_para=input_para->params+i*DIGEST_SIZE;
            if (i < input_para->param_num)
            {
                sscanf(curr_para,"%x",&vtcm_input->keyHandle); 
            }else{
                printf("Missing parameter for -ikh.\n");
                return -1;
            } 
        }else if (!strcmp("-idh",curr_para)) {
            i++;
            curr_para=input_para->params+i*DIGEST_SIZE;
            if (i < input_para->param_num)
            {
                sscanf(curr_para,"%x",&vtcm_input->EncryptAuthHandle); 
            }else{
                printf("Missing parameter for -idh.\n");
                return -1;
            } 
        } else if(!strcmp("-wf",curr_para)){
            i++;
            curr_para=input_para->params+i*DIGEST_SIZE;
            if(i<input_para->param_num){
                    keyfile=curr_para;            
            }

        }
        i++;
    }
    vtcm_input->tag = htons(TCM_TAG_RQU_AUTH1_COMMAND);
    vtcm_input->ordinal = SUBTYPE_SM4ENCRYPT_IN;
    memset(cbcModle,0,16);
    memcpy(vtcm_input->CBCusedIV,cbcModle,16);
    TCM_SESSION_DATA * authdata;
    authdata=Find_AuthSession(0x01,vtcm_input->EncryptAuthHandle);
    vtcm_input->EncryptDataSize =13 ; 
//   memcpy(vtcm_input->EncryptData,en4data,10);
    vtcm_input->EncryptData = Talloc0(vtcm_input->EncryptDataSize);
    if(vtcm_input->EncryptData==NULL)
     	return -EINVAL;

//    print_bin_data(Buf,datasize,8);
    Memcpy(vtcm_input->EncryptData,en4data,13);
    vtcm_template=memdb_get_template(DTYPE_VTCM_IN,SUBTYPE_SM4ENCRYPT_IN);
    if(vtcm_template==NULL)
        return -EINVAL;
    int offset=0;
    offset=struct_2_blob(vtcm_input,Buf,vtcm_template);
    if(offset<0)
        return offset;
    vtcm_input->paramSize=offset;
    int ordinal=htonl(vtcm_input->ordinal);
    int encryptdatasize=htonl(vtcm_input->EncryptDataSize);
    vtcm_SM3_2(hashout,&ordinal,4,vtcm_input->CBCusedIV,16,&encryptdatasize,4,vtcm_input->EncryptData,13);
    printf("%x\n",authdata->sharedSecret[0]);
    int serial = htonl(authdata->SERIAL);
    printf("%x\n",serial);
    vtcm_SM3_hmac(hmacout,authdata->sharedSecret,32,hashout,32,&serial,4);    
    memcpy(vtcm_input->EncryptAuthVerfication,hmacout,32);
    ret = struct_2_blob(vtcm_input,Buf,vtcm_template);
    if(ret<0)
        return ret;
    printf("Send command for sm4encrypt:\n");
    print_bin_data(Buf,ret,8);
    ret = vtcmutils_transmit(vtcm_input->paramSize,Buf,&outlen,Buf);
    if(ret<0)
        return ret;
    int fd;
    int length = outlen-46;
    unsigned char encData[length];
    for(i=0;i<length;i++){
        encData[i]=Buf[14+i];
    }
    fd=open(keyfile,O_CREAT|O_TRUNC|O_WRONLY,0666);
    if(fd<0){
        printf("file open error!\n");
        return -EIO;     
    }
    print_bin_data(encData,length,8);
    write(fd,encData,length);
    close(fd);
    printf("Receive  output is:\n");
    print_bin_data(Buf,outlen,8);
    return ret;
}
int proc_vtcmutils_SM4Decrypt(void * sub_proc, void * para){
    int outlen;
    int i=1;
    int ret=0;
    void *vtcm_template;
    struct tcm_in_Sm4Decrypt *vtcm_input;
    struct tcm_out_Sm4Decrypt *vtcm_output;
    unsigned char nonce[TCM_HASH_SIZE];
    unsigned char cbcModle[16];
    unsigned char hashout[TCM_HASH_SIZE];
    unsigned char hmacout[TCM_HASH_SIZE];
    char *data=NULL;
    vtcm_input = Talloc0(sizeof(*vtcm_input));
    if(vtcm_input==NULL)
        return -ENOMEM;
    vtcm_output = Talloc0(sizeof(*vtcm_output));
    if(vtcm_output==NULL)
        return -ENOMEM;
    char *keyfile=NULL;
    // insert data that will be encrypt and handle
    struct tcm_utils_input * input_para=para;
    char *curr_para;
    while (i<input_para->param_num) {
        curr_para=input_para->params+i*DIGEST_SIZE;
        if (!strcmp("-ikh",curr_para)) {
            i++;
            curr_para=input_para->params+i*DIGEST_SIZE;
            if (i < input_para->param_num)
            {
                sscanf(curr_para,"%x",&vtcm_input->keyHandle); 
            }else{
                printf("Missing parameter for -ikh.\n");
                return -1;
            } 
        }else if (!strcmp("-idh",curr_para)) {
            i++;
            curr_para=input_para->params+i*DIGEST_SIZE;
            if (i < input_para->param_num)
            {
                sscanf(curr_para,"%x",&vtcm_input->DecryptAuthHandle); 
            }else{
                printf("Missing parameter for -idh.\n");
                return -1;
            } 
        } else if(!strcmp("-rf",curr_para)){
            i++;
            curr_para=input_para->params+i*DIGEST_SIZE;
            if(i<input_para->param_num){
                    keyfile=curr_para;            
            }

        }
        i++;
    }
  /*  while (i<input_para->param_num) {
        curr_para=input_para->params+i*DIGEST_SIZE;
        if (!strcmp("-ih",curr_para)) {
            i++;
            curr_para=input_para->params+i*DIGEST_SIZE;
            if (i < input_para->param_num)
            {
                sscanf(curr_para,"%x",&vtcm_input->DecryptAuthHandle);
            }else{
                printf("Missing parameter for -ih.\n");
                return -1;
            } 
        }
        i++;
    }*/
    vtcm_input->tag = htons(TCM_TAG_RQU_AUTH1_COMMAND);
    vtcm_input->ordinal = SUBTYPE_SM4DECRYPT_IN;
    vtcm_input->keyHandle=0x05000001;
    memset(cbcModle,0,16);
    memcpy(vtcm_input->CBCusedIV,cbcModle,16);
   // memcpy(vtcm_input->EncryptData,data,strlen(data));
    vtcm_input->DecryptData=global_DecryptData;
    int fd;
    int datasize;
    TCM_SESSION_DATA * authdata;
    authdata=Find_AuthSession(0x01,vtcm_input->DecryptAuthHandle);
    fd=open(keyfile,O_RDONLY);
    if(fd<0)
         return -EIO;
    ret=read(fd,Buf,DIGEST_SIZE*32+1);
    if(ret<0)
        return -EIO;
    if(ret>DIGEST_SIZE*32)
    {
        printf("key file too large!\n");
        return -EINVAL;     
    }
    datasize=ret;
    vtcm_input->DecryptDataSize =datasize ; 
    vtcm_input->paramSize = datasize+70;
     //compute DecryptAuthVerfication
    vtcm_input->DecryptData = Talloc0(vtcm_input->DecryptDataSize);
    if(vtcm_input->DecryptData==NULL)
    	return -EINVAL;
    print_bin_data(Buf,datasize,8);
    Memcpy(vtcm_input->DecryptData,Buf,vtcm_input->DecryptDataSize);

    vtcm_template=memdb_get_template(DTYPE_VTCM_IN,SUBTYPE_SM4ENCRYPT_IN);
    if(vtcm_template==NULL)
        return -EINVAL;
    int offset=0;
    offset=struct_2_blob(vtcm_input,Buf,vtcm_template);
    if(offset<0)
        return offset;
    vtcm_input->DecryptDataSize=offset-70;
    vtcm_input->paramSize=offset;
    int ordinal=htonl(vtcm_input->ordinal);
    int decryptdatasize=htonl(vtcm_input->DecryptDataSize);
    vtcm_SM3_2(hashout,&ordinal,4,vtcm_input->CBCusedIV,16,vtcm_input->DecryptData,offset-70,&decryptdatasize,4);
    authdata=Find_AuthSession(0x04,vtcm_input->DecryptAuthHandle);
    int serial = htonl(authdata->SERIAL);
    vtcm_SM3_hmac(hmacout,authdata->sharedSecret,32,hashout,32,&serial,4);    
    memcpy(vtcm_input->DecryptAuthVerfication,hmacout,32);
    ret = struct_2_blob(vtcm_input,Buf,vtcm_template);
    if(ret<0)
        return ret;
    printf("Send command for sm4decrypt:\n");
    print_bin_data(Buf,ret,8);
    ret = vtcmutils_transmit(vtcm_input->paramSize,Buf,&outlen,Buf);
    if(ret<0)
        return ret; 
    printf("Receive  output is:\n");
    print_bin_data(Buf,outlen,8);
    return ret;
}
int proc_vtcmutils_SM3CompleteExtend(void * sub_proc, void * para){
    int outlen;
    int i=1;
    int ret=0;
    void *vtcm_template;
    struct tcm_in_Sm3CompleteExtend *vtcm_input;
    struct tcm_out_Sm3CompleteExtend *vtcm_output;
    unsigned char nonce[TCM_HASH_SIZE];
    char * datablock=NULL;
    vtcm_input = Talloc0(sizeof(*vtcm_input));
    if(vtcm_input==NULL)
        return -ENOMEM;
    vtcm_output = Talloc0(sizeof(*vtcm_output));
    if(vtcm_output==NULL)
        return -ENOMEM;
    struct tcm_utils_input * input_para=para;
    char *curr_para;
    while (i<input_para->param_num) {
        curr_para=input_para->params+i*DIGEST_SIZE;
        if (!strcmp("-ix",curr_para)) {
            i++;
            curr_para=input_para->params+i*DIGEST_SIZE;
            if (i < input_para->param_num)
            {
                sscanf(curr_para,"%d",&vtcm_input->pcrIndex);
            }else{
                printf("Missing parameter for -ix.\n");
                return -1;
            } 
        }else if(!strcmp(curr_para,"-im")){
            i++;
            curr_para=input_para->params+i*DIGEST_SIZE;
            if(i<input_para->param_num){
                datablock=curr_para;
            }
        }
        i++;
    }
    if(datablock!=NULL){
        vtcm_SM3(nonce,datablock,strlen(datablock));
    }
    vtcm_input->tag = htons(TCM_TAG_RQU_COMMAND);
    vtcm_input->ordinal = SUBTYPE_SM3COMPLETEEXTEND_IN;
    vtcm_input->dataBlock=nonce;
    vtcm_input->dataBlockSize=0x20;
    vtcm_template=memdb_get_template(DTYPE_VTCM_IN,SUBTYPE_SM3COMPLETEEXTEND_IN);
    if(vtcm_template==NULL)
        return -EINVAL;
    int offset=0;
    offset=struct_2_blob(vtcm_input,Buf,vtcm_template);
    if(offset<0)
        return offset;
    vtcm_input->paramSize=offset;
    ret = struct_2_blob(vtcm_input,Buf,vtcm_template);
    if(ret<0)
        return ret;
    printf("Send command for sm3completeextend:\n");
    print_bin_data(Buf,ret,8);
    ret = vtcmutils_transmit(vtcm_input->paramSize,Buf,&outlen,Buf);
    if(ret<0)
        return ret; 
    printf("Receive  output is:\n");
    print_bin_data(Buf,outlen,8);
    return ret;
}

int proc_vtcmutils_DisableForceClear(void * sub_proc, void * para)
{
    int i=1;
    int ret=0;
    int outlen;
    void *vtcm_template;
    struct tcm_in_DisableForceClear *vtcm_input;
    struct tcm_out_DisableForceClear *vtcm_output;
    vtcm_input = Talloc0(sizeof(*vtcm_input));
    if(vtcm_input==NULL)
        return -ENOMEM;
    vtcm_output = Talloc0(sizeof(*vtcm_output));
    if(vtcm_output==NULL)
        return -ENOMEM;
    vtcm_input->tag = htons(TCM_TAG_RQU_COMMAND);
    vtcm_input->ordinal= SUBTYPE_DISABLEFORCECLEAR_IN;
    vtcm_template=memdb_get_template(DTYPE_VTCM_IN,SUBTYPE_DISABLEFORCECLEAR_IN);
    if(vtcm_template==NULL)
        return -EINVAL;
    vtcm_input->paramSize=sizeof(*vtcm_input);
    ret = struct_2_blob(vtcm_input,Buf,vtcm_template);
    if(ret<0)
        return ret;
    printf("Send command for disableforceclear:\n");
    print_bin_data(Buf,ret,8);
    ret = vtcmutils_transmit(vtcm_input->paramSize,Buf,&outlen,Buf);
    if(ret<0)
        return ret; 
    printf("Receive  output is:\n");
    print_bin_data(Buf,outlen,8);
    return ret;
}

int proc_vtcmutils_ForceClear(void * sub_proc, void * para)
{
  int ret=0;
  int outlen;
  void *vtcm_template;
  struct tcm_in_ForceClear *vtcm_input;
  struct tcm_out_ForceClear *vtcm_output;
  vtcm_input = Talloc0(sizeof(*vtcm_input));
  if(vtcm_input==NULL)
  return -ENOMEM;
  vtcm_output = Talloc0(sizeof(*vtcm_output));
  if(vtcm_output==NULL)
  return -ENOMEM;
  vtcm_input->tag = htons(TCM_TAG_RQU_COMMAND);
  vtcm_input->ordinal= SUBTYPE_FORCECLEAR_IN;
  vtcm_template=memdb_get_template(DTYPE_VTCM_IN,SUBTYPE_FORCECLEAR_IN);
  if(vtcm_template==NULL)
  return -EINVAL;
  vtcm_input->paramSize=sizeof(*vtcm_input);
  ret = struct_2_blob(vtcm_input,Buf,vtcm_template);
  if(ret<0)
  return ret;
  printf("Send command for forceclear:\n");
  print_bin_data(Buf,ret,8);
  ret = vtcmutils_transmit(vtcm_input->paramSize,Buf,&outlen,Buf);
  if(ret<0)
  return ret; 
  printf("Receive  output is:\n");
  print_bin_data(Buf,outlen,8);
return ret;
}

int proc_vtcmutils_GetTestResult(void * sub_proc, void * para){
    int outlen;
    int ret=0;
    void *vtcm_template;
    struct tcm_in_GetTestResult *vtcm_input;
    struct tcm_out_GetTestResult *vtcm_output;
    vtcm_input = Talloc0(sizeof(*vtcm_input));
    if(vtcm_input==NULL)
        return -ENOMEM;
    vtcm_output = Talloc0(sizeof(*vtcm_output));
    if(vtcm_output==NULL)
        return -ENOMEM;
    vtcm_input->tag = htons(TCM_TAG_RQU_COMMAND);
    vtcm_input->ordinal= SUBTYPE_GETTESTRESULT_IN;
    vtcm_template=memdb_get_template(DTYPE_VTCM_IN,SUBTYPE_GETTESTRESULT_IN);
    if(vtcm_template==NULL)
        return -EINVAL;
    vtcm_input->paramSize=sizeof(*vtcm_input);
    ret = struct_2_blob(vtcm_input,Buf,vtcm_template);
    if(ret<0)
        return ret;
    printf("Send command for gettestresult:\n");
    print_bin_data(Buf,ret,8);
    ret = vtcmutils_transmit(vtcm_input->paramSize,Buf,&outlen,Buf);
    if(ret<0)
        return ret; 
    printf("Receive  output is:\n");
    print_bin_data(Buf,outlen,8);
    return ret;
}

int proc_vtcmutils_ContinueSelfTest(void * sub_proc, void * para)
{
    int outlen;
    int ret=0;
    void *vtcm_template;
    struct tcm_in_ContinueSelfTest *vtcm_input;
    struct tcm_out_ContinueSelfTest *vtcm_output;
    vtcm_input = Talloc0(sizeof(*vtcm_input));
    if(vtcm_input==NULL)
        return -ENOMEM;
    vtcm_output = Talloc0(sizeof(*vtcm_output));
    if(vtcm_output==NULL)
        return -ENOMEM;
    vtcm_input->tag = htons(TCM_TAG_RQU_COMMAND);
    vtcm_input->ordinal= SUBTYPE_CONTINUESELFTEST_IN;
    vtcm_template=memdb_get_template(DTYPE_VTCM_IN,SUBTYPE_CONTINUESELFTEST_IN);
    if(vtcm_template==NULL)
        return -EINVAL;
    vtcm_input->paramSize=sizeof(*vtcm_input);
    ret = struct_2_blob(vtcm_input,Buf,vtcm_template);
    if(ret<0)
        return ret;
    printf("Send command for continueselftest:\n");
    print_bin_data(Buf,ret,8);
    ret = vtcmutils_transmit(vtcm_input->paramSize,Buf,&outlen,Buf);
    if(ret<0)
        return ret; 
    printf("Receive  output is:\n");
    print_bin_data(Buf,outlen,8);
    return ret;
}

int proc_vtcmutils_SelfTestFull(void * sub_proc, void * para){
    int ret=0;
    int outlen;
    void *vtcm_template;
    struct tcm_in_SelfTestFull *vtcm_input;
    struct tcm_out_SelfTestFull *vtcm_output;
    vtcm_input = Talloc0(sizeof(*vtcm_input));
    if(vtcm_input==NULL)
        return -ENOMEM;
    vtcm_output = Talloc0(sizeof(*vtcm_output));
    if(vtcm_output==NULL)
        return -ENOMEM;
    vtcm_input->tag = htons(TCM_TAG_RQU_COMMAND);
    vtcm_input->ordinal= SUBTYPE_SELFTESTFULL_IN;
    vtcm_template=memdb_get_template(DTYPE_VTCM_IN,SUBTYPE_SELFTESTFULL_IN);
    if(vtcm_template==NULL)
        return -EINVAL;
    vtcm_input->paramSize=sizeof(*vtcm_input);
    ret = struct_2_blob(vtcm_input,Buf,vtcm_template);
    if(ret<0)
        return ret;
    printf("Send command for selftestfull:\n");
    print_bin_data(Buf,ret,8);
    ret = vtcmutils_transmit(vtcm_input->paramSize,Buf,&outlen,Buf);
    if(ret<0)
        return ret; 
    printf("Receive  output is:\n");
    print_bin_data(Buf,outlen,8);
    return ret;
}
/*int proc_vtcmutils_CertifyKey(void * sub_proc, void * para){
  int outlen;
  int i=0;
  int ret=0;
//Store anti-replay data
unsigned char nonce1[TCM_HASH_SIZE];
//Hold verification key handle
unsigned char handle1[4]={0x05,0x00,0x00,0x05};
//Store the key handle to be verified
unsigned char handle2[4]={0x05,0x00,0x00,0x06};
void *vtcm_template;
struct tcm_in_CertifyKey *vtcm_input;
struct tcm_out_CertifyKey *vtcm_output;
vtcm_input = Talloc0(sizeof(*vtcm_input));
if(vtcm_input==NULL)
return -ENOMEM;
vtcm_output = Talloc0(sizeof(*vtcm_output));
if(vtcm_output==NULL)
return -ENOMEM;
vtcm_input->tag = htons(TCM_TAG_RQU_AUTH_COMMAND);
vtcm_input->ordinal = SUBTYPE_CERTIFYKEY_IN;
memcpy(vtcm_input->keyhandle1,handle1,4);
memcpy(vtcm_input->keyhandle2,handle2,4);
TSS_gennonce(nonce1);
memcpy(vtcm_input->antidata,nonce1,TCM_HASH_SIZE);
vtcm_template=memdb_get_template(DTYPE_VTCM_IN,SUBTYPE_CERTIFYKEY_IN);
if(vtcm_template==NULL)
return -EINVAL;
vtcm_input->paramSize=sizeof(*vtcm_input);
ret = struct_2_blob(vtcm_input,Buf,vtcm_template);
if(ret<0)
return ret;
printf("Send command for certifykey:\n");
for(i=0;i<ret;i++){
printf("%.2x ",Buf[i]);
}	
printf("\n");
ret = vtcmutils_transmit(vtcm_input->paramSize,Buf,&outlen,Buf);
if(ret<0)
return ret; 
printf("Receive  output is:\n");
for(i=0;i<outlen;i++){
printf("%.2x ",Buf[i]);
if((i+1)%5==0)
printf("\n");
}
return ret;
}
int proc_vtcmutils_OwnerReadInternalPub(void * sub_proc, void * para){
int outlen;
int i=0;
int ret=0;
unsigned char nonce1[TCM_HASH_SIZE];
//Holds the shared secret data of the AP session created by owner
unsigned char nonce2[TCM_HASH_SIZE];
//AP session serial number
unsigned char nonce3[4]={0xf3,0x99,0x99,0xd4};
//Store EK keyhandle
unsigned char keyhandle[4]={0x40,0x00,0x00,0x06};
unsigned char hmac_out[TCM_HASH_SIZE];
void *vtcm_template;
struct tcm_in_OwnerReadInternalPub *vtcm_input;
struct tcm_out_OwnerReadInternalPub *vtcm_output;
vtcm_input = Talloc0(sizeof(*vtcm_input));
if(vtcm_input==NULL)
return -ENOMEM;
vtcm_output = Talloc0(sizeof(*vtcm_output));
if(vtcm_output==NULL)
return -ENOMEM;
vtcm_input->tag = htons(TCM_TAG_RQU_AUTH1_COMMAND);
vtcm_input->ordinal = SUBTYPE_OWNERREADINTERNALPUB_IN;
memcpy(vtcm_input->ekkeyhandle,keyhandle,4);
vtcm_input->authhandle=0x28;
vtcm_SM3_1(nonce1,(BYTE*)(vtcm_input->ordinal),sizeof(vtcm_input->oridnal),keyhandle,4);
memset(nonce2,0,TCM_HASH_SIZE);
vtcm_SM3_hmac(hmac_out,nonce2,TCM_HASH_SIZE,nonce1,TCM_HASH_SIZE,nonce3,4);
memcpy(vtcm_input->authcode,hamc_out,TCM_HASH_SIZE);
vtcm_template=memdb_get_template(DTYPE_VTCM_IN,SUBTYPE_OWNERREADINTERNALPUB_IN);
if(vtcm_template==NULL)
    return -EINVAL;
    vtcm_input->paramSize=0x32;
    ret = struct_2_blob(vtcm_input,Buf,vtcm_template);
if(ret<0)
    return ret;
    printf("Send command for ownerreadinternalpub:\n");
    for(i=0;i<ret;i++){
        printf("%.2x ",Buf[i]);
    }	
printf("\n");
ret = vtcmutils_transmit(vtcm_input->paramSize,Buf,&outlen,Buf);
if(ret<0)
    return ret; 
    printf("Receive  output is:\n");
    for(i=0;i<outlen;i++){
        printf("%.2x ",Buf[i]);
        if((i+1)%5==0)
            printf("\n");
    }
return ret;
}*/
int proc_vtcmutils_FlushSpecific(void * sub_proc, void * para){
    int outlen;
    int i=1;
    int ret=0;
    void *vtcm_template;
    TCM_AUTHHANDLE authhandle;
    struct tcm_in_FlushSpecific *vtcm_input;
    struct tcm_out_FlushSpecific *vtcm_output;
    vtcm_input = Talloc0(sizeof(*vtcm_input));
    if(vtcm_input==NULL)
        return -ENOMEM;
    vtcm_output = Talloc0(sizeof(*vtcm_output));
    if(vtcm_output==NULL)
        return -ENOMEM;
    vtcm_input->tag = htons(TCM_TAG_RQU_COMMAND);
    vtcm_input->ordinal = SUBTYPE_FLUSHSPECIFIC_IN;
     // Resource handle  variable
    struct tcm_utils_input * input_para=para;
    char * curr_para;
    curr_para=input_para->params+i*DIGEST_SIZE;
    if(!strcmp("-ih",curr_para)){
        i++;
        curr_para=input_para->params+i*DIGEST_SIZE;
        sscanf(curr_para,"%x",&vtcm_input->handle);
    }
    else{
        printf("sourcehandle is error\n");
    }
    // Resource type    variable
    vtcm_input->resourceType=0x01;
    vtcm_template=memdb_get_template(DTYPE_VTCM_IN,SUBTYPE_FLUSHSPECIFIC_IN);
    if(vtcm_template==NULL)
        return -EINVAL;
    vtcm_input->paramSize=sizeof(*vtcm_input);
    ret = struct_2_blob(vtcm_input,Buf,vtcm_template);
    if(ret<0)
        return ret;
    printf("Send command for FlushSpecific:\n");
    print_bin_data(Buf,ret,8);
    ret = vtcmutils_transmit(vtcm_input->paramSize,Buf,&outlen,Buf);
    if(ret<0)
        return ret; 
    printf("Receive  output is:\n");
    print_bin_data(Buf,outlen,8);
    return ret;
}
int proc_vtcmutils_DisableOwnerClear(void * sub_proc, void * para){
    int outlen;
    int i=1;
    int ret=0;
    TCM_SESSION_DATA * authdata;
    unsigned char nonce1[TCM_HASH_SIZE];
    //Holds the shared secret data of the AP session created by owner
    unsigned char nonce2[TCM_HASH_SIZE];
    //Store the serial number of the AP session
    unsigned char nonce3[4];
    unsigned char hmac_out[TCM_HASH_SIZE];
    void *vtcm_template;
    struct tcm_in_DisableOwnerClear *vtcm_input;
    struct tcm_out_DisableOwnerClear *vtcm_output;
    vtcm_input = Talloc0(sizeof(*vtcm_input));
    if(vtcm_input==NULL)
        return -ENOMEM;
    vtcm_output = Talloc0(sizeof(*vtcm_output));
    if(vtcm_output==NULL)
        return -ENOMEM;
    vtcm_input->tag = htons(TCM_TAG_RQU_AUTH1_COMMAND);
    vtcm_input->ordinal = SUBTYPE_DISABLEOWNERCLEAR_IN;
     //Authorize the session handle
     char * curr_para;
     struct tcm_utils_input * input_para=para;
     curr_para=input_para->params+i*DIGEST_SIZE;
     if(!strcmp("-ih",curr_para)){
         i++;
          curr_para=input_para->params+i*DIGEST_SIZE;
          sscanf(curr_para,"%x",&vtcm_input->authHandle);
     }else{
         printf("authhandle is error!\n");
     }
    int ordinal = htonl(vtcm_input->ordinal);
    vtcm_SM3(nonce1,&ordinal,4);
    authdata=Find_AuthSession(0x02,vtcm_input->authHandle);
    int serial = htonl(authdata->SERIAL);
    vtcm_SM3_hmac(hmac_out,authdata->sharedSecret,32,nonce1,32,&serial,4);
    memcpy(vtcm_input->ownerAuth,hmac_out,TCM_HASH_SIZE);
    vtcm_template=memdb_get_template(DTYPE_VTCM_IN,SUBTYPE_DISABLEOWNERCLEAR_IN);
    if(vtcm_template==NULL){
        printf("123\n");
        return -EINVAL;
    }
    vtcm_input->paramSize=sizeof(*vtcm_input);
    ret = struct_2_blob(vtcm_input,Buf,vtcm_template);
    if(ret<0)
        return ret;
    printf("Send command for DisableOwnerClear:\n");
    print_bin_data(Buf,ret,8);
    ret = vtcmutils_transmit(vtcm_input->paramSize,Buf,&outlen,Buf);
    if(ret<0)
        return ret; 
    printf("Receive  output is:\n");
    print_bin_data(Buf,outlen,8);
    return ret;
}

int proc_vtcmutils_OwnerClear(void * sub_proc, void * para){
    int outlen;
    int i=1;
    int ret=0;
    unsigned char nonce1[TCM_HASH_SIZE];
    //Holds the shared secret data of the AP session created by owner
    unsigned char nonce2[TCM_HASH_SIZE];
    //Store the serial number of the AP session
    unsigned char hmac_out[TCM_HASH_SIZE];
    void *vtcm_template;
    TCM_SESSION_DATA * authdata;
    struct tcm_in_OwnerClear *vtcm_input;
    struct tcm_out_OwnerClear *vtcm_output;
    vtcm_input = Talloc0(sizeof(*vtcm_input));
    if(vtcm_input==NULL)
        return -ENOMEM;
    vtcm_output = Talloc0(sizeof(*vtcm_output));
    if(vtcm_output==NULL)
        return -ENOMEM;
    vtcm_input->tag = htons(TCM_TAG_RQU_AUTH1_COMMAND);
    vtcm_input->ordinal = SUBTYPE_OWNERCLEAR_IN;
    //Authorize the session handle
     char * curr_para;
     struct tcm_utils_input * input_para=para;
     curr_para=input_para->params+i*DIGEST_SIZE;
    if(!strcmp("-ih",curr_para)){
        i++;
        curr_para=input_para->params+i*DIGEST_SIZE;
        sscanf(curr_para,"%x",&vtcm_input->authHandle);
    }else {
        printf("authhandle is error!\n");
    }
    int ordinal = htonl(vtcm_input->ordinal);
    vtcm_SM3(nonce1,&ordinal,4);
    print_bin_data(nonce1,32,8);
    authdata=Find_AuthSession(0x02,vtcm_input->authHandle);
   // memset(nonce2,0,TCM_HASH_SIZE);
    int serial = htonl(authdata->SERIAL);
    vtcm_SM3_hmac(hmac_out,authdata->sharedSecret,TCM_HASH_SIZE,nonce1,TCM_HASH_SIZE,&serial,4);
    print_bin_data(authdata->sharedSecret,32,8);
    memcpy(vtcm_input->ownerAuth,hmac_out,TCM_HASH_SIZE);
    print_bin_data(vtcm_input->ownerAuth,32,8);
    vtcm_template=memdb_get_template(DTYPE_VTCM_IN,SUBTYPE_OWNERCLEAR_IN);
    if(vtcm_template==NULL)
        return -EINVAL;
    vtcm_input->paramSize=sizeof(*vtcm_input);
    ret = struct_2_blob(vtcm_input,Buf,vtcm_template);
    if(ret<0)
        return ret;
    printf("Send command for OwnerClear:\n");
    print_bin_data(Buf,ret,8);
    ret = vtcmutils_transmit(vtcm_input->paramSize,Buf,&outlen,Buf);
    if(ret<0)
        return ret; 
    printf("Receive  output is:\n");
    print_bin_data(Buf,outlen,8);
    return ret;
}
int proc_vtcmutils_SM3Complete(void * sub_proc, void * para){
    int outlen;
    int i=1;
    int ret=0;
    int offset=0;
    void *vtcm_template;
    BYTE nonce[TCM_HASH_SIZE];
    char * datablock=NULL;
    struct tcm_in_Sm3Complete *vtcm_input;
    struct tcm_out_Sm3Complete *vtcm_output;
    struct tcm_utils_input *input_para=para;
    char *curr_para;
    vtcm_input = Talloc0(sizeof(*vtcm_input));
    if(vtcm_input==NULL)
        return -ENOMEM;
    vtcm_output = Talloc0(sizeof(*vtcm_output));
    if(vtcm_output==NULL)
        return -ENOMEM;
    vtcm_input->tag = htons(TCM_TAG_RQU_COMMAND);
    vtcm_input->ordinal = SUBTYPE_SM3COMPLETE_IN;
    while(i<input_para->param_num){
        curr_para=input_para->params+i*DIGEST_SIZE;
        if(!strcmp("-im",curr_para)){
            i++;
            curr_para=input_para->params+i*DIGEST_SIZE;
            datablock=curr_para;
        }
        i++;
    }
    if(datablock!=NULL){
        vtcm_SM3(nonce,datablock,strlen(datablock));
    }
    vtcm_input->dataBlock=nonce;
    vtcm_input->dataBlockSize=0x20;
    vtcm_template=memdb_get_template(DTYPE_VTCM_IN,SUBTYPE_SM3COMPLETE_IN);
    if(vtcm_template==NULL)
        return -EINVAL;
    offset = struct_2_blob(vtcm_input,Buf,vtcm_template);
    if(offset<0)
        return offset;
    vtcm_input->paramSize=offset;
    ret = struct_2_blob(vtcm_input,Buf,vtcm_template);
    if(ret<0)
        return ret;
    printf("Send command for sm3complete:\n");
    print_bin_data(Buf,ret,8);
    ret = vtcmutils_transmit(vtcm_input->paramSize,Buf,&outlen,Buf);
    if(ret<0)
        return ret; 
    printf("Receive  output is:\n");
    print_bin_data(Buf,outlen,8);
    return ret;
}
int proc_vtcmutils_SM3Update(void * sub_proc, void * para){
    int outlen;
    int i=1;
    int ret=0;
    int offset=0;
    void *vtcm_template;
    char *datablock=NULL;
    unsigned char nonce[TCM_HASH_SIZE];
    struct tcm_in_Sm3Update *vtcm_input;
    struct tcm_out_Sm3Update *vtcm_output;
    struct tcm_utils_input *input_para=para;
    char *curr_para;
    vtcm_input = Talloc0(sizeof(*vtcm_input));
    if(vtcm_input==NULL)
        return -ENOMEM;
    vtcm_output = Talloc0(sizeof(*vtcm_output));
    if(vtcm_output==NULL)
        return -ENOMEM;
    vtcm_input->tag = htons(TCM_TAG_RQU_COMMAND);
    vtcm_input->ordinal = SUBTYPE_SM3UPDATE_IN;
    while(i<input_para->param_num){
        curr_para=input_para->params+i*DIGEST_SIZE;
        if(!strcmp("-im",curr_para)){
            i++;
            curr_para=input_para->params+i*DIGEST_SIZE;
            datablock=curr_para;
        }
        i++;
    }
    if(datablock!=NULL){
        vtcm_SM3(nonce,datablock,strlen(datablock));
    }
    vtcm_input->dataBlock=nonce;
   // memcpy(vtcm_input->dataBlock,nonce,TCM_HASH_SIZE);
    vtcm_input->dataBlockSize=0x20;
    vtcm_template=memdb_get_template(DTYPE_VTCM_IN,SUBTYPE_SM3UPDATE_IN);
    if(vtcm_template==NULL)
        return -EINVAL;
    offset = struct_2_blob(vtcm_input,Buf,vtcm_template);
    if(offset<0)
        return offset;
    vtcm_input->paramSize=offset;
    ret = struct_2_blob(vtcm_input,Buf,vtcm_template);
    if(ret<0)
        return ret;
    printf("Send command for sm3Update:\n");
    print_bin_data(Buf,ret,8);
    ret = vtcmutils_transmit(vtcm_input->paramSize,Buf,&outlen,Buf);
    if(ret<0)
        return ret; 
    printf("Receive  output is:\n");
    print_bin_data(Buf,outlen,8);
    return ret;
}
int proc_vtcmutils_SM3Start(void * sub_proc, void * para){
    int outlen;
    int i=0;
    int ret=0;
    void *vtcm_template;
    struct tcm_in_Sm3Start *vtcm_input;
    struct tcm_out_Sm3Start *vtcm_output;
    vtcm_input = Talloc0(sizeof(*vtcm_input));
    if(vtcm_input==NULL)
        return -ENOMEM;
    vtcm_output = Talloc0(sizeof(*vtcm_output));
    if(vtcm_output==NULL)
        return -ENOMEM;
    vtcm_input->tag = htons(TCM_TAG_RQU_COMMAND);
    vtcm_input->ordinal = SUBTYPE_SM3START_IN;
    vtcm_template=memdb_get_template(DTYPE_VTCM_IN,SUBTYPE_SM3START_IN);
    if(vtcm_template==NULL)
        return -EINVAL;
    vtcm_input->paramSize=sizeof(*vtcm_input);
    ret = struct_2_blob(vtcm_input,Buf,vtcm_template);
    if(ret<0)
        return ret;
    printf("Send command for sm3start:\n");
    print_bin_data(Buf,ret,8);
    ret = vtcmutils_transmit(vtcm_input->paramSize,Buf,&outlen,Buf);
    if(ret<0)
        return ret; 
    printf("Receive  output is:\n");
    print_bin_data(Buf,outlen,8);
    return ret;
}
int proc_vtcmutils_StartUp(void * sub_proc, void * para){
    int outlen;
    int i=1;
    int ret=0;
    void *vtcm_template;
    struct tcm_in_Startup *vtcm_input;
    struct tcm_out_Startup *vtcm_output;
    vtcm_input = Talloc0(sizeof(*vtcm_input));
    if(vtcm_input==NULL)
        return -ENOMEM;
    vtcm_output = Talloc0(sizeof(*vtcm_output));
    if(vtcm_output==NULL)
        return -ENOMEM;
    vtcm_input->tag = htons(TCM_TAG_RQU_COMMAND);
    vtcm_input->ordinal = SUBTYPE_STARTUP_IN;
    struct tcm_utils_input * input_para=para;
    char * curr_para;
    curr_para=input_para->params+i*DIGEST_SIZE;
    if(!strcmp("-is",curr_para)){
        i++;
        curr_para=input_para->params+i*DIGEST_SIZE;
        if(!strcmp("clear",curr_para)){
            vtcm_input->startupType=0x01;
        }
        else if(!strcmp("state",curr_para)){
            vtcm_input->startupType=0x02;
        }
        else if(!strcmp("deavtived",curr_para)){
            vtcm_input->startupType=0x03;
        }else{
            printf("Invalid startup type!\n");
        }
    }else{
        printf("-is is an invalid parameter!\n");
        return -1;
    }
    vtcm_template=memdb_get_template(DTYPE_VTCM_IN,SUBTYPE_STARTUP_IN);
    if(vtcm_template==NULL)
        return -EINVAL;
    vtcm_input->paramSize=sizeof(*vtcm_input);
    ret = struct_2_blob(vtcm_input,Buf,vtcm_template);
    if(ret<0)
        return ret;
    printf("Send command for startup:\n");
    print_bin_data(Buf,ret,8);
    ret = vtcmutils_transmit(vtcm_input->paramSize,Buf,&outlen,Buf);
    if(ret<0)
        return ret; 
    printf("Receive  startup message output is:\n");
    print_bin_data(Buf,outlen,8);
    return ret;
}
int proc_vtcmutils_getRandom(void * sub_proc, void * para){
    int outlen;
    int i=0;
    int ret=0;
    void *vtcm_template;
    struct tcm_in_GetRandom *vtcm_input;
    struct tcm_out_GetRandom *vtcm_output;
    vtcm_input = Talloc0(sizeof(*vtcm_input));
    if(vtcm_input==NULL)
        return -ENOMEM;
    vtcm_output = Talloc0(sizeof(*vtcm_output));
    if(vtcm_output==NULL)
        return -ENOMEM;
    vtcm_input->tag = htons(TCM_TAG_RQU_COMMAND);
    vtcm_input->ordinal = SUBTYPE_GETRANDOM_IN;
    vtcm_input->bytesRequested=0x10;
    vtcm_template=memdb_get_template(DTYPE_VTCM_IN,SUBTYPE_GETRANDOM_IN);
    if(vtcm_template==NULL)
        return -EINVAL;
    vtcm_input->paramSize=sizeof(*vtcm_input);
    ret = struct_2_blob(vtcm_input,Buf,vtcm_template);
    if(ret<0)
        return ret;
    printf("Send command for getRandom:\n");
    print_bin_data(Buf,ret,8);
    ret = vtcmutils_transmit(vtcm_input->paramSize,Buf,&outlen,Buf);
    if(ret<0)
        return ret; 
    printf("Receive  output is:\n");
    print_bin_data(Buf,outlen,8);
    return ret;
} 
int proc_vtcmutils_getcapability(void * sub_proc, void * para){
    int outlen;
    int i=1;
    int ret=0;
    void *vtcm_template;
    char * curr_para;
    unsigned char nonce[TCM_HASH_SIZE];
    struct tcm_utils_input * input_para=para;
    struct tcm_in_GetCapability * vtcm_input;
    struct tcm_out_GetCapability * vtcm_output;
    vtcm_input = Talloc0(sizeof(*vtcm_input));
    if(vtcm_input==NULL)
        return -ENOMEM;
    vtcm_output = Talloc0(sizeof(*vtcm_output));
    curr_para=input_para->params+i*DIGEST_SIZE;
    if(vtcm_output==NULL)
        return -ENOMEM;
    vtcm_input->tag = htons(TCM_TAG_RQU_COMMAND);
    vtcm_input->ordinal = SUBTYPE_GETCAPABILITY_IN;
    vtcm_template=memdb_get_template(DTYPE_VTCM_IN,SUBTYPE_GETCAPABILITY_IN);
    if(vtcm_template==NULL)
        return -EINVAL;
     curr_para=input_para->params+i*DIGEST_SIZE;
    if(!strcmp("ord",curr_para)){
        vtcm_input->capArea=0x01;
        vtcm_input->subCap=NULL;
        vtcm_input->subCapSize=0;
        vtcm_input->paramSize=0x12;
    }
    else if(!strcmp("propery",curr_para)){
        vtcm_input->capArea=0x05;
        vtcm_input->subCap=NULL;
        vtcm_input->subCapSize=0;
        vtcm_input->paramSize=0x12;
    }
    else if(!strcmp("keyhandle",curr_para)){
        vtcm_input->capArea=0x07;
        vtcm_input->subCap=NULL;
        vtcm_input->subCapSize=0;
        vtcm_input->paramSize=0x12;
    }
    else if(!strcmp("version",curr_para)){
        vtcm_input->capArea=0x06;
        vtcm_input->subCap=NULL;
        vtcm_input->subCapSize=0;
        vtcm_input->paramSize=0x12;
    }
    else if(!strcmp("versionval",curr_para)){
        vtcm_input->capArea=0x1a;
        vtcm_input->subCap=NULL;
        vtcm_input->subCapSize=0;
        vtcm_input->paramSize=0x12;
    }else{
        printf("error\n");
    }
    ret = struct_2_blob(vtcm_input,Buf,vtcm_template);
    if(ret<0)
        return ret;
    printf("Send command for getcapability %s:\n",curr_para);
    print_bin_data(Buf,ret,8);
    ret = vtcmutils_transmit(vtcm_input->paramSize,Buf,&outlen,Buf);
    if(ret<0)
        return ret;
    printf("Receive  output is:\n");
    print_bin_data(Buf,outlen,8);
    return ret;
}

int proc_vtcmutils_takeownership(void * sub_proc, void * para)
{
    int outlen;
    int i=2;
    int ret=0;
    void *vtcm_template;
    unsigned char nonce[TCM_HASH_SIZE];
    struct tcm_utils_input * input_para=para;
    struct tcm_in_TakeOwnership * vtcm_input;
    struct tcm_out_TakeOwnership * vtcm_output;
    char * pwdo="ooo";
    char * pwds="sss";
    BYTE ownerauth[TCM_HASH_SIZE];
    BYTE smkauth[TCM_HASH_SIZE];
    int  enclen=512;
    TCM_SYMMETRIC_KEY_PARMS * sm4_parms;
    TCM_SESSION_DATA * authdata;
    vtcm_input = Talloc0(sizeof(*vtcm_input));
    if(vtcm_input==NULL)
        return -ENOMEM;
    vtcm_output = Talloc0(sizeof(*vtcm_output));
    if(vtcm_output==NULL)
        return -ENOMEM;
    vtcm_input->tag = htons(TCM_TAG_RQU_AUTH1_COMMAND);
    vtcm_input->ordinal = SUBTYPE_TAKEOWNERSHIP_IN;
    vtcm_input->protocolID=htons(TCM_PID_OWNER);
    int offset=0;

    authdata = Find_AuthSession(TCM_ET_NONE,0);
    if(authdata==NULL)
    {
        printf("can't find session for takeownership!\n");
        return -EINVAL;
    }	

    sm3(pwdo,Strlen(pwdo),ownerauth);
    sm3(pwds,Strlen(pwds),smkauth);

    // compute ownerAuth
    enclen=512;
    ret=GM_SM2Encrypt(Buf,&enclen,ownerauth,TCM_HASH_SIZE,pubEK->pubKey.key,pubEK->pubKey.keyLength);
    if(ret!=0)
        return ret; 


    vtcm_input->encOwnerAuthSize=enclen;
    vtcm_input->encOwnerAuth=Talloc0(enclen);
    Memcpy(vtcm_input->encOwnerAuth,Buf,enclen);

    // compute smkAuth
    enclen=512;
    ret=GM_SM2Encrypt(Buf,&enclen,smkauth,TCM_HASH_SIZE,pubEK->pubKey.key,pubEK->pubKey.keyLength);
    if(ret!=0)
        return ret; 

    vtcm_input->encSmkAuthSize=enclen;
    vtcm_input->encSmkAuth=Talloc0(enclen);
    Memcpy(vtcm_input->encSmkAuth,Buf,enclen);

    //  add vtcm_input's smkParams


    vtcm_input->smkParams.tag=htons(TCM_TAG_KEY);
    vtcm_input->smkParams.keyUsage=TCM_SM4KEY_STORAGE;
    vtcm_input->smkParams.keyFlags=0;
    vtcm_input->smkParams.authDataUsage=TCM_AUTH_ALWAYS;
    vtcm_input->smkParams.algorithmParms.algorithmID=TCM_ALG_SM4;
    vtcm_input->smkParams.algorithmParms.encScheme=TCM_ES_SM4_CBC;
    vtcm_input->smkParams.algorithmParms.sigScheme=TCM_SS_NONE;

    vtcm_input->smkParams.PCRInfoSize=0;
    vtcm_input->smkParams.PCRInfo=NULL;
    vtcm_input->smkParams.pubKey.keyLength=0;
    vtcm_input->smkParams.pubKey.key=NULL;
    vtcm_input->smkParams.encDataSize=0;
    vtcm_input->smkParams.encData=NULL;

    // add smkparms's sm4 key parms
    sm4_parms=Talloc0(sizeof(*sm4_parms));
    if(sm4_parms==NULL)
        return -ENOMEM;
    sm4_parms->keyLength=0x80;
    sm4_parms->blockSize=0x80;
    sm4_parms->ivSize=0x10;
    sm4_parms->IV=Talloc0(sm4_parms->ivSize);
    vtcm_template=memdb_get_template(DTYPE_VTCM_IN_KEY,SUBTYPE_TCM_BIN_SYMMETRIC_KEY_PARMS);
    if(vtcm_template==NULL)
        return -EINVAL;

    ret=struct_2_blob(sm4_parms,Buf,vtcm_template);
    if(ret<0)
        return ret;	

    vtcm_input->smkParams.algorithmParms.parmSize=ret;
    vtcm_input->smkParams.algorithmParms.parms=Talloc0(ret);
    if(vtcm_input->smkParams.algorithmParms.parms==NULL)
        return -ENOMEM;
    Memcpy(vtcm_input->smkParams.algorithmParms.parms,Buf,ret);





    // add authHandle
    vtcm_input->authHandle=authdata->handle;			

    vtcm_template=memdb_get_template(DTYPE_VTCM_IN,SUBTYPE_TAKEOWNERSHIP_IN);
    if(vtcm_template==NULL)
        return -EINVAL;
    offset = struct_2_blob(vtcm_input,Buf,vtcm_template);
    if(offset<0)
        return offset;

    // compute authCode
    sm3(Buf+6,offset-6-36,vtcm_input->authCode);

    Memcpy(Buf,vtcm_input->authCode,TCM_HASH_SIZE);

    uint32_t temp_int=htonl(vtcm_input->authHandle);

    Memcpy(Buf+TCM_HASH_SIZE,&temp_int,sizeof(temp_int));

    sm3_hmac(ownerauth,TCM_HASH_SIZE,Buf,TCM_HASH_SIZE+sizeof(uint32_t),vtcm_input->authCode);


    vtcm_input->paramSize=offset;
    //    Memcpy(Buf+ (int)(&(((struct tcm_in_TakeOwnership *)0)->paramSize)),&vtcm_input->paramSize,sizeof(vtcm_input->paramSize)); 
    printf("Begin input for takeownership:\n");
    offset = struct_2_blob(vtcm_input,Buf,vtcm_template);
    if(offset<0)
        return offset;

    print_bin_data(Buf,offset,8);
    ret = vtcmutils_transmit(vtcm_input->paramSize,Buf,&outlen,Buf);
    if(ret<0)
        return ret;
    printf("takeownership:\n");
    print_bin_data(Buf,outlen,8);
    return ret;
}

int proc_vtcmutils_PhysicalSetDeactivated(void * sub_proc, void * para){
    int outlen;
    int i=1;
    int ret=0;
    void *vtcm_template;
    struct tcm_utils_input * input_para=para;
    struct tcm_in_PhysicalSetDeactivated * vtcm_input;
    struct tcm_out_PhysicalSetDeactivated * vtcm_output;
    vtcm_input = Talloc0(sizeof(*vtcm_input));
    if(vtcm_input==NULL)
        return -ENOMEM;
    vtcm_output = Talloc0(sizeof(*vtcm_output));
    if(vtcm_output==NULL)
        return -ENOMEM;
    vtcm_input->tag = htons(TCM_TAG_RQU_COMMAND);
    vtcm_input->ordinal = SUBTYPE_PHYSICALSETDEACTIVATED_IN;
    char * curr_para;
    curr_para=input_para->params+i*DIGEST_SIZE;
    if(!strcmp("-is",curr_para)){
        i++;
        curr_para=input_para->params+i*DIGEST_SIZE;
        if(!strcmp("true",curr_para)){
            vtcm_input->state = TRUE;
        }
        else if(!strcmp("false",curr_para)){
            vtcm_input->state = FALSE;
        }
        else{
            printf("Invalid status bit!\n");
        }
    }else{
        printf("Parametr is error!\n");
    }
    vtcm_template=memdb_get_template(DTYPE_VTCM_IN,SUBTYPE_PHYSICALSETDEACTIVATED_IN);
    if(vtcm_template==NULL)
        return -EINVAL;
    vtcm_input->paramSize=sizeof(*vtcm_input);
    ret = struct_2_blob(vtcm_input,Buf,vtcm_template);
    if(ret<0)
        return ret;
    printf("Begin input for physicalsetdeactivate:\n");
    print_bin_data(Buf,ret,8);
    ret = vtcmutils_transmit(vtcm_input->paramSize,Buf,&outlen,Buf);
    if(ret<0)
        return ret;
    printf("Physicasetdeactivate output is:\n");
    print_bin_data(Buf,outlen,8);
    return ret;
}

int proc_vtcmutils_PhysicalDisable(void * sub_proc, void * para){
    int outlen;
    int i=2;
    int ret=0;
    void *vtcm_template;
    struct tcm_in_PhysicalDisable * vtcm_input;
    struct tcm_out_PhysicalDisable * vtcm_output;
    vtcm_input = Talloc0(sizeof(*vtcm_input));
    if(vtcm_input==NULL)
        return -ENOMEM;
    vtcm_output = Talloc0(sizeof(*vtcm_output));
    if(vtcm_output==NULL)
        return -ENOMEM;
    vtcm_input->tag = htons(TCM_TAG_RQU_COMMAND);
    vtcm_input->ordinal = SUBTYPE_PHYSICALDISABLE_IN;
    vtcm_template=memdb_get_template(DTYPE_VTCM_IN,SUBTYPE_PHYSICALDISABLE_IN);
    if(vtcm_template==NULL)
        return -EINVAL;
    vtcm_input->paramSize=sizeof(*vtcm_input);
    ret = struct_2_blob(vtcm_input,Buf,vtcm_template);
    if(ret<0)
        return ret;
    printf("Begin input for physicaldisable:\n");
    print_bin_data(Buf,ret,8);
    ret = vtcmutils_transmit(vtcm_input->paramSize,Buf,&outlen,Buf);
    if(ret<0)
        return ret;
    printf("Physicaldisable output is:\n");
    print_bin_data(Buf,outlen,8);
    return ret;
}

int proc_vtcmutils_PhysicalEnable(void * sub_proc, void * para){
    int outlen;
    int i;
    int ret=0;
    void *vtcm_template;
    struct tcm_in_PhysicalEnable * vtcm_input;
    struct tcm_out_PhysicalEnable * vtcm_output;
    vtcm_input = Talloc0(sizeof(*vtcm_input));
    if(vtcm_input==NULL)
        return -ENOMEM;
    vtcm_output = Talloc0(sizeof(*vtcm_output));
    if(vtcm_output==NULL)
        return -ENOMEM;
    vtcm_input->tag = htons(TCM_TAG_RQU_COMMAND);
    vtcm_input->ordinal = SUBTYPE_PHYSICALENABLE_IN;
    vtcm_template=memdb_get_template(DTYPE_VTCM_IN,SUBTYPE_PHYSICALENABLE_IN);
    if(vtcm_template==NULL)
        return -EINVAL;
    vtcm_input->paramSize=sizeof(*vtcm_input);
    ret = struct_2_blob(vtcm_input,Buf,vtcm_template);
    if(ret<0)
        return ret;
    printf("Begin input for physicalenable :\n");
    print_bin_data(Buf,ret,8);
    ret = vtcmutils_transmit(vtcm_input->paramSize,Buf,&outlen,Buf);
    if(ret<0)
        return ret;
    printf("Physicalenable output is:\n");
    print_bin_data(Buf,outlen,8);
    return ret;
}
int proc_vtcmutils_APTerminate(void * sub_proc, void * para){
    int outlen;
    int i=1;
    int ret=0;
    void *vtcm_template;
    struct tcm_in_APTerminate *vtcm_input;
    struct tcm_out_APTerminate *vtcm_output;
    unsigned char key[TCM_HASH_SIZE];
    unsigned char checknum[TCM_HASH_SIZE];
    unsigned char hashout[TCM_HASH_SIZE];
    TCM_SESSION_DATA * authdata;
    vtcm_input = Talloc0(sizeof(*vtcm_input));
    if(vtcm_input==NULL)
        return -ENOMEM;
    vtcm_output = Talloc0(sizeof(*vtcm_output));
    if(vtcm_output==NULL)
        return -ENOMEM;
    vtcm_input->tag = htons(TCM_TAG_RQU_AUTH1_COMMAND);
    vtcm_input->ordinal = SUBTYPE_APTERMINATE_IN;
    struct tcm_utils_input * input_para=para;
    char * curr_para;
    curr_para=input_para->params+i*DIGEST_SIZE;
    if(!strcmp("-ih",curr_para)){
        i++;
        curr_para=input_para->params+i*DIGEST_SIZE;
        sscanf(curr_para,"%x",&vtcm_input->authHandle);
    }else{
        printf("authhandle is error\n");
    }
    int ordinal = htonl(vtcm_input->ordinal);
    vtcm_SM3(checknum,&ordinal,4);
    authdata=Find_AuthSession(0x00,vtcm_input->authHandle);
    int serial = htonl(authdata->SERIAL);
    vtcm_SM3_hmac(hashout,authdata->sharedSecret,32,checknum,32,&serial,4);
    memcpy(vtcm_input->authCode,hashout,TCM_HASH_SIZE);
    vtcm_template=memdb_get_template(DTYPE_VTCM_IN,SUBTYPE_APTERMINATE_IN);
    if(vtcm_template==NULL)
        return -EINVAL;
    vtcm_input->paramSize=sizeof(*vtcm_input);
    ret = struct_2_blob(vtcm_input,Buf,vtcm_template);
    if(ret<0)
        return ret;
    printf("Send command for APTerminal:\n");
    print_bin_data(Buf,ret,8);
    ret = vtcmutils_transmit(vtcm_input->paramSize,Buf,&outlen,Buf);
    if(ret<0)
        return ret; 
    printf("Receive  output is:\n");
    print_bin_data(Buf,outlen,8);
    return ret;
}
/*
int proc_vtcmutils_WrapKey(void * sub_proc, void * para){
    int outlen;
    int i=1;
    int ret = 0;
    int offset=0;
    char *keyfile=NULL;
    void * vtcm_template;
    unsigned char ownerauth[TCM_HASH_SIZE];
    unsigned char migrationauth[TCM_HASH_SIZE];
    unsigned char nonce[TCM_HASH_SIZE];
    unsigned char hashout[TCM_HASH_SIZE];
    unsigned char pubAuth[TCM_HASH_SIZE];
    unsigned char hmacout[TCM_HASH_SIZE];
    TCM_AUTH_SESSION_DATA * authdata;
    struct tcm_in_WrapKey * vtcm_input;
    struct tcm_out_WrapKey * vtcm_output;
    char *pwdo="ooo";
    char *pwdm="mmm";
    TCM_SYMMETRIC_KEY_PARMS *sm4_parms;
    TCM_AUTH_SESSION_DATA * authdata;
    vtcm_input = Talloc0(sizeof(*vtcm_input));
    if(vtcm_input==NULL)
        return -ENOMEM;
    vtcm_output = Talloc0(sizeof(*vtcm_output));
    if(vtcm_output==NULL)
        return -ENOMEM;
    vtcm_input->tag = htons(TCM_TAG_RQU_AUTH1_COMMAND);
    vtcm_input->ordinal = SUBTYPE_WRAPKEY_IN;
    struct tcm_utils_input * input_para=para;
    char *curr_para;
    while (i<input_para->param_num) {
        curr_para=input_para->params+i*DIGEST_SIZE;
        if (!strcmp("-ikh",curr_para)) {
            i++;
            curr_para=input_para->params+i*DIGEST_SIZE;
            if (i < input_para->param_num)
            {
                sscanf(curr_para,"%x",&vtcm_input->parentHandle); 
            }else{
                printf("Missing parameter for -ikh.\n");
                return -1;
            } 
        }else if (!strcmp("-idh",curr_para)) {
            i++;
            curr_para=input_para->params+i*DIGEST_SIZE;
            if (i < input_para->param_num)
            {
                sscanf(curr_para,"%x",&vtcm_input->authHandle); 
            }else{
                printf("Missing parameter for -idh.\n");
                return -1;
            } 
        }else if(!strcmp(curr_para,"-rf")){
            i++;
            curr_para=input_para->params+i*DIGEST_SIZE;
            if(i<input_para->param_num){
               keyfile=curr_para;
            }
        }
        i++;
    }
    //compute ownerauthdata and migrationauthdata
    sm3(pwdo,strlen(pwdo),ownerauth);
    sm3(pwdm.strlen(pwdm),migrationauth);
    Memcpy(vtcm_input->dataUsageAuth,ownerauth,TCM_HASH_SIZE);	
    Memcpy(vtcm_input->dataMigrationAuth,migrationauth,TCM_HASH_SIZE); 
	int fd;
	int datasize;
	fd=open(keyfile,O_RDONLY);
	if(fd<0)
		return -EIO;
	ret=read(fd,Buf,DIGEST_SIZE*32+1);
	if(ret<0)
		return -EIO;
	if(ret>DIGEST_SIZE*32)
	{
		printf("key file too large!\n");
		return -EINVAL;
	}
	datasize=ret;
	vtcm_template=memdb_get_template(DTYPE_VTCM_IN_KEY,SUBTYPE_TCM_BIN_KEY);
	if(vtcm_template==NULL)
	{
		return -EINVAL;
	}
	ret=blob_2_struct(Buf,&vtcm_input->keyInfo,vtcm_template);
	if((ret<0)||(ret>datasize))
	{
		printf("read key file error!\n");
		return -EINVAL;
	}
    //compute keyInfo
 /*   vtcm_input->keyInfo.tag=htons(TCM_TAG_KEY);
    vtcm_input->keyInfo.keyUsage=TCM_SM$KEY_STORAGE;
    vtcm_input->keyInfo.keyFlags=0;
    vtcm_input->keyInfo.authDataUsage=TCM_AUTH_ALWAYS;

    vtcm_input->keyInfo.algorithmParms.algorithmID=TCM_ALG_SM4;
    vtcm_input->keyInfo.algorithmParms.encScheme=TCM_ES_SM4_CBC;
    vtcm_input->keyInfo.algorithmParms.sigScheme=TCM_SS_NONE;

    vtcm_input->keyInfo.PCRInfoSize=0;
    vtcm_input->keyInfo.PCRInfo=NULL;
    vtcm_input->keyInfo.pubKey.keyLength=0;
    vtcm_input->keyInfo.pubKey.key=NULL;
    vtcm_input->keyInfo.encDataSize=0;
    vtcm_input->keyInfo.encData=NULL;
    // add smkparms's sm4 key parms
    sm4_parms=Talloc0(sizeof(*sm4_parms));
    if(sm4_parms==NULL)
        return -ENOMEM;
    sm4_parms->keyLength=0x80;
    sm4_parms->blockSize=0x80;
    sm4_parms->ivSize=0x10;
    sm4_parms->IV=Talloc0(sm4_parms->ivSize);
    vtcm_template=memdb_get_template(DTYPE_VTCM_IN_KEY,SUBTYPE_TCM_BIN_SYMMETRIC_KEY_PARMS);
    if(vtcm_template==NULL)
        return -EINVAL;
    ret=struct_2_blob(sm4_parms,Buf,vtcm_template);
    if(ret<0)
        return ret; 
    vtcm_input->keyInfo.algorithmParms.parmSize=ret;
    vtcm_input->keyInfo.algorithmParms.parms=Talloc0(ret);
    if(vtcm_input->keyInfo.algorithmParms.parms==NULL)
        return -ENOMEM;
    Memcpy(vtcm_input->keyInfo.algorithmParms.parms,Buf,ret);*/
    // compute keyInfo size
    //  apt-get install nfs-kernel-server 
   /* vtcm_template=memdb_get_template(DTYPE_VTCM_IN_KEY,SUBTYPE_WRAPKEY_IN);
    offset =  struct_2_blob(vtcm_input,Buf,vtcm_template);
    int ordinal = htonl(vtcm_input->ordinal);
    vtcm_SM3_2(hashout,&(ordinal),4,vtcm_input->dataUsageAuth,32,vtcm_input->dataMigrationAuth,32,vtcm_input->keyInfo,ret);
    authdata=Find_AuthSession(0x04,vtcm_input->authHandle);
    int serial = htonl(authdata->SERIAL);
    vtcm_SM3_hmac(hmacout,authdata->sharedSecret,32,hashout,32,serial,4);
    Memcpy(vtcm_input->pubAuth,hmacout,TCM_HASH_SIZE); 
    vtcm_input->paramSize=offset;
    vtcm_template=memdb_get_template(DTYPE_VTCM_IN,SUBTYPE_WRAPKEY_IN);
    if(vtcm_template==NULL)
        return -EINVAL;
    ret =  struct_2_blob(vtcm_input,Buf,vtcm_template);
    if(ret<0)
        return ret;
    print_bin_data(Buf,ret,8);
    ret = vtcmutils_transmit(vtcm_input->paramSize,Buf,&outlen,Buf);
    if(ret<0)
        return ret;
    print_bin_data(Buf,outlen,8);
    vtcm_template=memdb_get_template(DTYPE_VTCM_OUT,SUBTYPE_WRAPKEY_OUT);
    if(vtcm_template==NULL)
        return -EINVAL;
    ret=blob_2_struct(Buf,vtcm_output,vtcm_template);
    if(ret<0)
        return ret;

    ret=struct_2_json(vtcm_output,Buf,vtcm_template);
    if(ret<0)
        return ret;
    printf("%s\n",Buf);
    return ret;
}*/
int vtcm_AuthSessionData_Encrypt(BYTE *retData,
                                 TCM_SESSION_DATA *authSession,
                                 BYTE *encData
                                )
{
    int ret = 0; 

    sm4_context ctx; 
            
    BYTE *sessionKey = (BYTE *)malloc(sizeof(BYTE)*TCM_NONCE_SIZE);
    ret = KDFwithSm3(sessionKey, authSession->sharedSecret, TCM_NONCE_SIZE/2, TCM_NONCE_SIZE);
    if(ret != 0)
    {    
        printf("Error, KDFwithSm3\n");
    }
    sm4_setkey_enc(&ctx, sessionKey);
    sm4_crypt_ecb(&ctx, 1, 32, encData, retData);
    return ret;
}

int proc_vtcmutils_CreateWrapKey(void * sub_proc, void * para){
    int outlen;
    int i=1;
    int fd;
    int ret = 0;
    int offset=0;
    void * vtcm_template;
    TCM_AUTHHANDLE authhandle;
    unsigned char ownerauth[TCM_HASH_SIZE];
    unsigned char migrationauth[TCM_HASH_SIZE];
    unsigned char nonce[TCM_HASH_SIZE];
    unsigned char hashout[TCM_HASH_SIZE];
    unsigned char pubAuth[TCM_HASH_SIZE];
    unsigned char APKey[TCM_HASH_SIZE];
    unsigned char hmacout[TCM_HASH_SIZE];
    unsigned char authdata1[TCM_HASH_SIZE];
    unsigned char migrationdata[TCM_HASH_SIZE];
    char *pwdk="kkk";
    char *pwdm="mmm";
    TCM_SESSION_DATA * authdata;
    struct tcm_in_CreateWrapKey *vtcm_input;
    struct tcm_out_CreateWrapKey *vtcm_output;

    struct tcm_utils_input * input_para=para;
    char * index_para;
    char * value_para;
    char * keyfile=NULL;
    char * select=NULL;

    sm3(pwdk,strlen(pwdk),authdata1);
    sm3(pwdm,strlen(pwdm),migrationdata);
    TCM_SYMMETRIC_KEY_PARMS *sm4_parms;
    TCM_SM2_ASYMKEY_PARAMETERS *sm2_parms;
    vtcm_input = Talloc0(sizeof(*vtcm_input));
    if(vtcm_input==NULL)
        return -ENOMEM;
    vtcm_output = Talloc0(sizeof(*vtcm_output));
    if(vtcm_output==NULL)
        return -ENOMEM;
    vtcm_input->tag = htons(TCM_TAG_RQU_AUTH1_COMMAND);
    vtcm_input->ordinal = SUBTYPE_CREATEWRAPKEY_IN;
    vtcm_input->parentHandle=0x40;

	if((input_para->param_num>0)&&
		(input_para->param_num%2==1))
	{
		for(i=1;i<input_para->param_num;i+=2)
		{
        		index_para=input_para->params+i*DIGEST_SIZE;
        		value_para=index_para+DIGEST_SIZE;
			if(!Strcmp("-ih",index_para))
			{
        			sscanf(value_para,"%x",&vtcm_input->authHandle);
			}	
			else if(!Strcmp("-is",index_para))
			{
				select=value_para;
			}
			else if(!Strcmp("-kf",index_para))
			{
				keyfile=value_para;
			}
			else
			{
				printf("Error cmd format! should be %s -ih smkauthhandle -is keyusage -kf keyfile"
					"[-pwd passwd]",input_para->params);
				return -EINVAL;
			}
		}
	}	
    //Fill keyInfo information
    vtcm_input->keyInfo.tag=htons(TCM_TAG_KEY);
    vtcm_input->keyInfo.keyFlags=0;
    vtcm_input->keyInfo.authDataUsage=TCM_AUTH_ALWAYS;
    
    if(!strcmp("sm4",select))
    {
        vtcm_input->keyInfo.keyUsage=TCM_SM4KEY_STORAGE;
        vtcm_input->keyInfo.algorithmParms.algorithmID=TCM_ALG_SM4;
        vtcm_input->keyInfo.algorithmParms.encScheme=TCM_ES_SM4_CBC;
        vtcm_input->keyInfo.algorithmParms.sigScheme=TCM_SS_NONE;
        printf("this is sm4\n");
        // add smkparms's sm4 key parms
        sm4_parms=Talloc0(sizeof(*sm4_parms));
        if(sm4_parms==NULL)
            return -ENOMEM;
        sm4_parms->keyLength=0x80;
        sm4_parms->blockSize=0x80;
        sm4_parms->ivSize=0x10;
        sm4_parms->IV=Talloc0(sm4_parms->ivSize);
        vtcm_template=memdb_get_template(DTYPE_VTCM_IN_KEY,SUBTYPE_TCM_BIN_SYMMETRIC_KEY_PARMS);
        if(vtcm_template==NULL)
            return -EINVAL;
        ret=struct_2_blob(sm4_parms,Buf,vtcm_template);
        if(ret<0)
            return ret; 
        vtcm_input->keyInfo.algorithmParms.parmSize=ret;
        vtcm_input->keyInfo.algorithmParms.parms=Talloc0(ret);
        if(vtcm_input->keyInfo.algorithmParms.parms==NULL)
            return -ENOMEM;
        Memcpy(vtcm_input->keyInfo.algorithmParms.parms,Buf,ret);
    }else
    {
        vtcm_input->keyInfo.keyUsage=TCM_SM2KEY_IDENTITY;
        //add smkparms's sm2 key parms
        sm2_parms=Talloc0(sizeof(*sm2_parms));
        if(sm2_parms==NULL)
            return -ENOMEM;
        sm2_parms->keyLength=0x80;
        vtcm_template=memdb_get_template(DTYPE_VTCM_IN_KEY,SUBTYPE_TCM_BIN_SM2_ASYMKEY_PARAMETERS);
        if(vtcm_template==NULL)
                    return -EINVAL;
        ret=struct_2_blob(sm2_parms,Buf,vtcm_template);
            if(ret<0)
                        return ret;
        vtcm_input->keyInfo.algorithmParms.parmSize=ret;
        vtcm_input->keyInfo.algorithmParms.parms=Talloc0(ret);
        if(vtcm_input->keyInfo.algorithmParms.parms==NULL)
            return -ENOMEM;
        Memcpy(vtcm_input->keyInfo.algorithmParms.parms,Buf,ret);
        vtcm_input->keyInfo.algorithmParms.algorithmID=TCM_ALG_SM2;
        vtcm_input->keyInfo.algorithmParms.encScheme=TCM_ES_SM2;
        vtcm_input->keyInfo.algorithmParms.sigScheme=TCM_SS_SM2;
        printf("this is sm2\n");
    }

    vtcm_input->keyInfo.PCRInfoSize=0;
    vtcm_input->keyInfo.PCRInfo=NULL;
    vtcm_input->keyInfo.pubKey.keyLength=0;
    vtcm_input->keyInfo.pubKey.key=NULL;
    vtcm_input->keyInfo.encDataSize=0;
    vtcm_input->keyInfo.encData=NULL;

    BYTE *Buffer=(BYTE*)malloc(sizeof(BYTE)*256);
    vtcm_template=memdb_get_template(DTYPE_VTCM_IN_KEY,SUBTYPE_TCM_BIN_KEY);
    if(vtcm_template==NULL)
        return -EINVAL;
    int ret1=0;
    ret1=struct_2_blob(&(vtcm_input->keyInfo),Buffer,vtcm_template);
    if(ret1<0)
        return ret1;
    vtcm_template=memdb_get_template(DTYPE_VTCM_IN,SUBTYPE_CREATEWRAPKEY_IN);
    if(vtcm_template==NULL)
        return -EINVAL;
    offset=struct_2_blob(vtcm_input,Buf,vtcm_template);
    printf("%d\n",offset);
  //  memset(APKey,0,TCM_HASH_SIZE);
    authdata=Find_AuthSession(0x04,vtcm_input->authHandle);
    vtcm_AuthSessionData_Encrypt(&ownerauth,authdata,authdata1);
    Memcpy(vtcm_input->dataUsageAuth,ownerauth,TCM_HASH_SIZE);	
    vtcm_AuthSessionData_Encrypt(&migrationauth,authdata,migrationdata);
    Memcpy(vtcm_input->dataMigrationAuth,migrationauth,TCM_HASH_SIZE);
    int ordinal=htonl(vtcm_input->ordinal);
    vtcm_SM3_2(hashout,&(ordinal),4,vtcm_input->dataUsageAuth,TCM_HASH_SIZE,vtcm_input->dataMigrationAuth,TCM_HASH_SIZE,
               Buffer,ret1);
    int serial = htonl(authdata->SERIAL);
    printf("%x\n",serial);
    vtcm_SM3_hmac(hmacout,authdata->sharedSecret,32,hashout,32,&serial,4);
    Memcpy(vtcm_input->pubAuth,hmacout,TCM_HASH_SIZE); 
    vtcm_input->paramSize=offset;
    printf("Begin input for CreateWrapKey\n");
    offset =  struct_2_blob(vtcm_input,Buf,vtcm_template);
    if(offset<0)
        return offset;
    print_bin_data(Buf,offset,8);

    ret = vtcmutils_transmit(vtcm_input->paramSize,Buf,&outlen,Buf);
    if(ret<0)
        return ret;
    print_bin_data(Buf,outlen,8);
    vtcm_template=memdb_get_template(DTYPE_VTCM_OUT,SUBTYPE_CREATEWRAPKEY_OUT);
    if(vtcm_template==NULL)
        return -EINVAL;
    ret=blob_2_struct(Buf,vtcm_output,vtcm_template);
    if(ret<0)
        return ret;

    vtcm_template=memdb_get_template(DTYPE_VTCM_IN_KEY,SUBTYPE_TCM_BIN_KEY);
    if(vtcm_template==NULL)
	return -EINVAL;	
 
    // write keyfile	

    ret=struct_2_blob(&vtcm_output->wrappedKey,Buf,vtcm_template);
    if(ret<0)
	return -EINVAL;
    fd=open(keyfile,O_CREAT|O_TRUNC|O_WRONLY,0666);
    if(fd<0)
	return -EIO;
    write(fd,Buf,ret);
    close(fd);

    sprintf(Buf,"%d \n",vtcm_output->returnCode);

   void * send_msg =vtcm_auto_build_outputmsg(Buf,NULL);

   if(send_msg==NULL)
	return -EINVAL;

   ex_module_sendmsg(sub_proc,send_msg);		

    return ret;
}


int proc_vtcmutils_LoadKey(void * sub_proc, void * para){
    int outlen;
    int i=1;
    int ret = 0;
    int offset=0;
    void * vtcm_template;
    TCM_AUTHHANDLE authhandle;
    unsigned char loadkey[TCM_HASH_SIZE];
    unsigned char nonce[TCM_HASH_SIZE];
    unsigned char hashout[TCM_HASH_SIZE];
    unsigned char pubAuth[TCM_HASH_SIZE];
    unsigned char APKey[TCM_HASH_SIZE];
    unsigned char hmacout[TCM_HASH_SIZE];
    TCM_SESSION_DATA * authdata;
    struct tcm_in_LoadKey * vtcm_input;
    struct tcm_out_LoadKey * vtcm_output;
    vtcm_input = Talloc0(sizeof(*vtcm_input));
    if(vtcm_input==NULL)
        return -ENOMEM;
    vtcm_output = Talloc0(sizeof(*vtcm_output));
    if(vtcm_output==NULL)
        return -ENOMEM;
    vtcm_input->tag = htons(TCM_TAG_RQU_AUTH1_COMMAND);
    vtcm_input->ordinal = SUBTYPE_LOADKEY_IN;
    vtcm_input->parentHandle=0x40;
    struct tcm_utils_input * input_para=para;
    char * index_para;
    char * value_para;
    char * keyfile=NULL;

	if((input_para->param_num>0)&&
		(input_para->param_num%2==1))
	{
		for(i=1;i<input_para->param_num;i+=2)
		{
        		index_para=input_para->params+i*DIGEST_SIZE;
        		value_para=index_para+DIGEST_SIZE;
			if(!Strcmp("-ih",index_para))
			{
        			sscanf(value_para,"%x",&vtcm_input->authHandle);
			}	
			else if(!Strcmp("-kf",index_para))
			{
				keyfile=value_para;
			}
			else
			{
				printf("Error cmd format! should be %s -ih authHandle -kf keyfile",input_para->params);
				return -EINVAL;
			}
		}
    	}

	int fd;
	int datasize;
	fd=open(keyfile,O_RDONLY);
	if(fd<0)
		return -EIO;
	ret=read(fd,Buf,DIGEST_SIZE*32+1);
	if(ret<0)
		return -EIO;
	if(ret>DIGEST_SIZE*32)
	{
		printf("key file too large!\n");
		return -EINVAL;
	}
	datasize=ret;
	vtcm_template=memdb_get_template(DTYPE_VTCM_IN_KEY,SUBTYPE_TCM_BIN_KEY);
	if(vtcm_template==NULL)
	{
		return -EINVAL;
	}
	ret=blob_2_struct(Buf,&vtcm_input->inKey,vtcm_template);
	if((ret<0)||(ret>datasize))
	{
		printf("read key file error!\n");
		return -EINVAL;
	}

    //Fill keyinfo information
/*
    vtcm_input->inKey.tag=global_tcm_key->tag;
    vtcm_input->inKey.keyUsage=global_tcm_key->keyUsage;
    vtcm_input->inKey.keyFlags=global_tcm_key->keyFlags;
    vtcm_input->inKey.authDataUsage=global_tcm_key->authDataUsage;
    vtcm_input->inKey.algorithmParms.algorithmID=global_tcm_key->algorithmParms.algorithmID;
    vtcm_input->inKey.algorithmParms.encScheme=global_tcm_key->algorithmParms.encScheme;
    vtcm_input->inKey.algorithmParms.sigScheme=global_tcm_key->algorithmParms.sigScheme;
    vtcm_input->inKey.PCRInfoSize=global_tcm_key->PCRInfoSize;
    vtcm_input->inKey.PCRInfo=global_tcm_key->PCRInfo;
    vtcm_input->inKey.pubKey.keyLength=global_tcm_key->pubKey.keyLength;
    vtcm_input->inKey.pubKey.key=global_tcm_key->pubKey.key;
    vtcm_input->inKey.encDataSize=global_tcm_key->encDataSize;
    vtcm_input->inKey.encData=global_tcm_key->encData;
    vtcm_input->inKey.algorithmParms.parmSize=global_tcm_key->algorithmParms.parmSize;
    vtcm_input->inKey.algorithmParms.parms=global_tcm_key->algorithmParms.parms;
*/
   // int inkey1= htonl(vtcm_input->inKey);
    BYTE *Buffer=(BYTE*)malloc(sizeof(BYTE)*256);
    vtcm_template=memdb_get_template(DTYPE_VTCM_IN_KEY,SUBTYPE_TCM_BIN_KEY);
    if(vtcm_template==NULL)
        return -EINVAL;
    int ret1=0;
    ret1=struct_2_blob(&(vtcm_input->inKey),Buffer,vtcm_template);
    if(ret1<0)
        return ret; 
    int ordinal=htonl(vtcm_input->ordinal);
    vtcm_SM3_1(hashout,&ordinal,4,Buffer,ret1);
    authdata=Find_AuthSession(0x04,vtcm_input->authHandle);
    int serial = htonl(authdata->SERIAL);
    vtcm_SM3_hmac(hmacout,authdata->sharedSecret,TCM_HASH_SIZE,hashout,TCM_HASH_SIZE,&serial,4);
    Memcpy(vtcm_input->parentAuth,hmacout,TCM_HASH_SIZE); 
    vtcm_template=memdb_get_template(DTYPE_VTCM_IN,SUBTYPE_LOADKEY_IN);
    if(vtcm_template==NULL)
        return -EINVAL;
    offset=struct_2_blob(vtcm_input,Buf,vtcm_template);
    if(offset<0)
        return offset;
    vtcm_input->paramSize=offset;
    
    printf("Begin input for LoadKey\n");
    print_bin_data(Buf,offset,8);
    ret = vtcmutils_transmit(vtcm_input->paramSize,Buf,&outlen,Buf);
    if(ret<0){
        return ret;
    }
    print_bin_data(Buf,outlen,8);
    printf("%d\n",outlen);
    vtcm_template=memdb_get_template(DTYPE_VTCM_OUT,SUBTYPE_LOADKEY_OUT);
    if(vtcm_template==NULL)
        return -EINVAL;
    ret=blob_2_struct(Buf,vtcm_output,vtcm_template);
    if(ret<0)
        return ret;

    // Output the info
    printf("KeyHandle %x\n",vtcm_output->inKeyHandle);
    sprintf(Buf,"%d %x\n",vtcm_output->returnCode,vtcm_output->inKeyHandle);

   void * send_msg =vtcm_auto_build_outputmsg(Buf,NULL);

   if(send_msg==NULL)
	return -EINVAL;

   ex_module_sendmsg(sub_proc,send_msg);		
    return ret;
}

int proc_vtcmutils_APCreate(void * sub_proc, void * para){
    int outlen;
    int i=1;
    int ret = 0;
    void * vtcm_template;
    TCM_AUTHHANDLE authhandle;
    unsigned char nonce[TCM_HASH_SIZE];
    unsigned char nonce1[TCM_HASH_SIZE];
    unsigned char key[TCM_HASH_SIZE];
    unsigned char auth[TCM_HASH_SIZE];
    struct tcm_in_APCreate * vtcm_input;
    struct tcm_out_APCreate * vtcm_output;
    TCM_SESSION_DATA * authdata;
    vtcm_input = Talloc0(sizeof(*vtcm_input));
    if(vtcm_input==NULL)
        return -ENOMEM;
    vtcm_output = Talloc0(sizeof(*vtcm_output));
    if(vtcm_output==NULL)
        return -ENOMEM;
    vtcm_input->tag = htons(TCM_TAG_RQU_AUTH1_COMMAND);
    vtcm_input->ordinal = SUBTYPE_APCREATE_IN;
    char * pwdo="ooo";
    char * pwds="sss";
    char * pwdk="kkk";
    struct tcm_utils_input * input_para=para;
    struct tcm_utils_output * output_para;
    char * curr_para;
    curr_para=input_para->params+i*DIGEST_SIZE;
    if(!strcmp("-it",curr_para)){
        i++;
        curr_para=input_para->params+i*DIGEST_SIZE;
        if(!strcmp("12",curr_para)){
            //nonce
            vtcm_input->entityType=0x12;
            vtcm_input->entityValue=0;
            memset(auth,0,TCM_HASH_SIZE);
        }else if(!strcmp("01",curr_para)){
            //keyhandle
            vtcm_input->entityType=0x01;
            vtcm_input->entityValue=0;
            i++;
             curr_para=input_para->params+i*DIGEST_SIZE;
             if(strcmp("-iv",curr_para))
	     {
		printf("Error cmd format! Need entityValue!\n");
		return -EINVAL;
             }
             i++;
             curr_para=input_para->params+i*DIGEST_SIZE;
	     sscanf(curr_para,"%x",&vtcm_input->entityValue);
	
            sm3(pwdk,strlen(pwdk),auth);
        }else if(!strcmp("04",curr_para)){
            // smk
            vtcm_input->entityType=0x04;
            vtcm_input->entityValue=0;
            sm3(pwds,Strlen(pwds),auth);
        }else if(!strcmp("0b",curr_para)){
            // NV
            vtcm_input->entityType=0x0b;
            vtcm_input->entityValue=0;
        }else if(!strcmp("05",curr_para)){
            // key related
            vtcm_input->entityType=0x05;
            vtcm_input->entityValue=0;
        }else if(!strcmp("02",curr_para)){
            // owner
            vtcm_input->entityType=0x02;
            vtcm_input->entityValue=0;
            sm3(pwdo,strlen(pwdk),auth);
    }else{
        printf("type is error");
    }
    }else{
        printf("parameter is error!\n");
    }
   // memset(EAData,0,TCM_HASH_SIZE);
    print_bin_data(auth,TCM_HASH_SIZE,TCM_HASH_SIZE/4);
    int ordinal=htonl(vtcm_input->ordinal);
    UINT16 entityType=htons(vtcm_input->entityType);
    vtcm_SM3_1(key,&(ordinal),sizeof(vtcm_input->ordinal),&(entityType),sizeof(vtcm_input->entityType));
    i=0;
    TSS_gennonce(nonce);
    Memcpy(vtcm_input->nonce,nonce,TCM_HASH_SIZE);	
    vtcm_SM3_hmac(nonce1,auth,32,key,32,nonce,32);
    Memcpy(vtcm_input->authCode,nonce1,TCM_HASH_SIZE);

    authdata=Create_AuthSession_Data(&(vtcm_input->entityType),vtcm_input->authCode,vtcm_input->nonce);

    vtcm_template=memdb_get_template(DTYPE_VTCM_IN,SUBTYPE_APCREATE_IN);
    if(vtcm_template==NULL)
        return -EINVAL;
    vtcm_input->paramSize=0x50;
    ret =  struct_2_blob(vtcm_input,Buf,vtcm_template);
    if(ret<0)
        return ret;
    print_bin_data(Buf,ret,8);
    ret = vtcmutils_transmit(vtcm_input->paramSize,Buf,&outlen,Buf);
    if(ret<0)
        return ret;
    print_bin_data(Buf,outlen,8);
    vtcm_template=memdb_get_template(DTYPE_VTCM_OUT,SUBTYPE_APCREATE_OUT);
    if(vtcm_template==NULL)
        return -EINVAL;
   ret=blob_2_struct(Buf,vtcm_output,vtcm_template);
   if(ret<0)
     return ret;

   ret=struct_2_json(vtcm_output,Buf,vtcm_template);
   if(ret<0)
        return ret;
    printf("%s\n",Buf);
    memcpy(authdata->sharedSecret, auth, TCM_NONCE_SIZE); 
    authhandle=Build_AuthSession(authdata,vtcm_output);	
    if(authhandle==0)
        return -EINVAL;	

    sprintf(Buf,"%d %x\n",vtcm_output->returnCode,vtcm_output->authHandle);

   void * send_msg =vtcm_auto_build_outputmsg(Buf,NULL);

   if(send_msg==NULL)
	return -EINVAL;

   ex_module_sendmsg(sub_proc,send_msg);		

   return ret;
}

int proc_vtcmutils_readPubek(void * sub_proc, void * para){
    int outlen;
    int i=0;
    int ret = 0;
    struct tcm_in_ReadPubek * vtcm_input;
    struct tcm_out_ReadPubek * vtcm_output;
    void * vtcm_template;
    unsigned char nonce[TCM_HASH_SIZE];
    printf("Begin readpubek:\n");
    vtcm_input = Talloc0(sizeof(*vtcm_input));
    if(vtcm_input==NULL)
        return -ENOMEM;
    vtcm_output = Talloc0(sizeof(*vtcm_output));
    if(vtcm_output==NULL)
        return -ENOMEM;
    vtcm_input->tag = htons(TCM_TAG_RQU_COMMAND);
    vtcm_input->ordinal=SUBTYPE_READPUBEK_IN;
    TSS_gennonce(nonce);
    Memcpy(vtcm_input->antiReplay,nonce,TCM_HASH_SIZE);
    vtcm_template=memdb_get_template(DTYPE_VTCM_IN,SUBTYPE_READPUBEK_IN);
    if(vtcm_template==NULL)
        return -EINVAL;
    vtcm_input->paramSize=sizeof(*vtcm_input);
    ret =  struct_2_blob(vtcm_input,Buf,vtcm_template);
    if(ret<0)
        return ret;

    print_bin_data(Buf,ret,8);

    BYTE *BBuffer = (BYTE *)malloc(sizeof(BYTE) * 512) ;
    ret = vtcmutils_transmit(vtcm_input->paramSize,Buf,&outlen,BBuffer);
    if(ret<0)
        return ret;
    vtcm_template=memdb_get_template(DTYPE_VTCM_OUT,SUBTYPE_READPUBEK_OUT);
    if(vtcm_template==NULL)
        return -EINVAL;
    ret=blob_2_struct(BBuffer,vtcm_output,vtcm_template);
    if(ret<0)
        return ret;

    print_bin_data(vtcm_output->pubEndorsementKey.pubKey.key,vtcm_output->pubEndorsementKey.pubKey.keyLength,8);

    pubEK=Dalloc0(sizeof(*pubEK),sub_proc);
    if(pubEK==NULL)
        return -ENOMEM;
    vtcm_template=memdb_get_template(DTYPE_VTCM_IN_KEY,SUBTYPE_TCM_BIN_PUBKEY);
    if(vtcm_template==NULL)
        return -EINVAL;

    ret=struct_clone(&vtcm_output->pubEndorsementKey,pubEK,vtcm_template);

    return ret;
}
int proc_vtcmutils_createEKPair(void * sub_proc, void * para){
    int outlen;
    int i=1;
    int ret = 0;
    struct tcm_in_CreateEKPair * vtcm_input;
    struct tcm_out_CreateEKPair * vtcm_output;
    TCM_SM2_ASYMKEY_PARAMETERS * key_parms_in;
    void * vtcm_template;
    void * vtcm_template1;
    char * ekfile=NULL;
    struct tcm_utils_input * input_para=para;
    char * curr_para;
    while(i<input_para->param_num){                                        
        curr_para=input_para->params+i*DIGEST_SIZE;
      if(!strcmp("-wf",curr_para)){
                  i++;
                  curr_para=input_para->params+i*DIGEST_SIZE;
                  ekfile=curr_para;     
     }
          i++;
    }

    unsigned char nonce[TCM_HASH_SIZE];
    printf("Begin Create EK Pair:\n");
    vtcm_input = Talloc0(sizeof(*vtcm_input));
    if(vtcm_input==NULL)
        return -ENOMEM;
    vtcm_output = Talloc0(sizeof(*vtcm_output));
    if(vtcm_output==NULL)
        return -ENOMEM;
    key_parms_in = Talloc0(sizeof(*key_parms_in));
    if(key_parms_in==NULL)
        return -ENOMEM;
    vtcm_input->tag = htons(TCM_TAG_RQU_COMMAND);
    vtcm_input->ordinal=SUBTYPE_CREATEEKPAIR_IN;
    TSS_gennonce(nonce);
    while(i<TCM_HASH_SIZE){
        vtcm_input->antiReplay[i]=(BYTE)nonce[i];
        i++;
    }
    vtcm_input->keyInfo.algorithmID=TCM_ALG_SM2;
    vtcm_input->keyInfo.encScheme=TCM_ES_SM2;
    vtcm_input->keyInfo.sigScheme=TCM_SS_SM2;
    key_parms_in->keyLength=256;
    vtcm_template=memdb_get_template(DTYPE_VTCM_IN_KEY,SUBTYPE_TCM_BIN_SM2_ASYMKEY_PARAMETERS);
    if(vtcm_template==NULL)
        return -EINVAL;
    vtcm_input->keyInfo.parmSize=4;
    ret = struct_2_blob(key_parms_in,Buf,vtcm_template);
    if(ret<0)
        return ret;
    vtcm_input->keyInfo.parms=Buf;

    //	vtcm_input->keyInfo.parmSize=sizeof(*vtcm_input->keyInfo.parms);
    vtcm_template1=memdb_get_template(DTYPE_VTCM_IN,SUBTYPE_CREATEEKPAIR_IN);
    if(vtcm_template1==NULL)
        return -EINVAL;
    vtcm_input->paramSize=sizeof(*vtcm_output)-sizeof(BYTE *)+vtcm_input->keyInfo.parmSize;
    ret = 0;
    BYTE *BBuffer = (BYTE *)malloc(sizeof(BYTE) * 1000) ;
    ret = struct_2_blob(vtcm_input,BBuffer,vtcm_template1);
    if(ret<0)
        return ret;
    print_bin_data(BBuffer,ret,8);
    BYTE *BBuffer_1 = (BYTE *)malloc(sizeof(BYTE) * 1000) ;
    ret = vtcmutils_transmit(vtcm_input->paramSize,BBuffer,&outlen,BBuffer_1);
    if(ret<0)
        return ret;
    vtcm_template1=memdb_get_template(DTYPE_VTCM_OUT,SUBTYPE_CREATEEKPAIR_OUT);
    if(vtcm_template1==NULL)
        return -EINVAL;
    ret = blob_2_struct(BBuffer,vtcm_output,vtcm_template);
    int fd;
    fd = open(ekfile,O_CREAT|O_TRUNC|O_WRONLY,0666);
    if(fd<0)
         return -EIO;
    vtcm_template=memdb_get_template(DTYPE_VTCM_IN_KEY,SUBTYPE_TCM_BIN_PUBKEY);
    ret = struct_2_blob(&(vtcm_output->pubEndorsementKey),Buf,vtcm_template);
    if(ret<0)
        return -EIO;
    write(fd,Buf,ret);
    printf("CreateEKPair is:\n");
    print_bin_data(BBuffer_1,outlen,8);
    return ret;
}
int proc_vtcmutils_Extend(void * sub_proc, void * para){
    int i=1;
    int j=0;
    int index = -1;
    int outlen;
    int ret = 0;
    char * message = NULL;
    struct tcm_in_extend * vtcm_input;
    struct tcm_out_extend * vtcm_output;
    void * vtcm_template;
    unsigned char msghash[32];
    printf("Begin extend:\n");
    vtcm_input = Talloc0(sizeof(*vtcm_input));
    if(vtcm_input==NULL)
        return -ENOMEM;
    vtcm_output = Talloc0(sizeof(*vtcm_output));
    if(vtcm_output==NULL)
        return -ENOMEM;
    struct tcm_utils_input * input_para=para;
    char * curr_para;

    while(i<input_para->param_num){
        curr_para=input_para->params+i*DIGEST_SIZE;
        if(!strcmp("-ix",curr_para)){
            i++;
            curr_para=input_para->params+i*DIGEST_SIZE;
            if(i<input_para->param_num){
                sscanf(curr_para,"%d",&vtcm_input->pcrNum);
                //	index = Atoi(input_para->argv[i]);
            }
            else{
                printf("Missing parameter for -ix.\n");
                return -1;
            }	
        }else if(!strcmp(curr_para,"-ic")){
            i++;
            curr_para=input_para->params+i*DIGEST_SIZE;
            if(i<input_para->param_num){
                message = curr_para;
            }else{
                printf("Missing parameter for -ic.\n");
            }
        }
        i++;
    }
    if(message != NULL){
        vtcm_SM3(msghash, message,strlen(message));
    }
    for(j=0;j<32;j++){
        vtcm_input->inDigest[j] = (BYTE)msghash[j];
    }
    vtcm_input->tag = htons(TCM_TAG_RQU_COMMAND);
    vtcm_input->ordinal=SUBTYPE_EXTEND_IN;
    vtcm_template=memdb_get_template(DTYPE_VTCM_IN,SUBTYPE_EXTEND_IN);
    if(vtcm_template==NULL)
        return -EINVAL;
    vtcm_input->paramSize=sizeof(*vtcm_input);
    ret =  struct_2_blob(vtcm_input,Buf,vtcm_template);
    if(ret<0)
        return ret;
    print_bin_data(Buf,ret,8);
    ret = vtcmutils_transmit(vtcm_input->paramSize,Buf,&outlen,Buf);
    if(ret<0){
        return ret;}
    vtcm_template=memdb_get_template(DTYPE_VTCM_OUT,SUBTYPE_EXTEND_OUT);
    if(vtcm_template==NULL)
        return -EINVAL;
    ret=blob_2_struct(Buf,vtcm_output,vtcm_template);
    if(ret<0)
        return ret;
    i = 0;
    printf("New value of PCR %d:\n",vtcm_input->pcrNum);
    while(i<TCM_HASH_SIZE){
        printf("%.2x ",vtcm_output->outDigest[i]);
        i++;
    }
    printf("\n");
    return ret;
}
int proc_vtcmutils_PcrReset(void * sub_proc, void * para)
{
    int i = 1;
    int ret = 0;
    int b = 0;
    int index = -1;
    int outlen;
    struct tcm_in_pcrreset * vtcm_input;
    struct tcm_out_pcrreset * vtcm_output;
    void * vtcm_template;
    struct tcm_utils_input * input_para=para;
    char * curr_para;
    printf("begin proc pcrreset \n");
    vtcm_input=Talloc0(sizeof(*vtcm_input));
    if(vtcm_input==NULL)
        return -ENOMEM;	
    vtcm_output=Talloc0(sizeof(*vtcm_output));
    if(vtcm_output==NULL)
        return -ENOMEM;	
    vtcm_input->pcrSelection.sizeOfSelect=0x03;
    while (i < input_para->param_num) {
        curr_para=input_para->params+i*DIGEST_SIZE;
        if (!strcmp("-ix",curr_para)) {
            i++;
            curr_para=input_para->params+i*DIGEST_SIZE;
            if (i < input_para->param_num)
            {
                // sscanf(curr_para,"%d",&vtcm_input->pcrIndex);
                index=atoi(curr_para);
                b=(index>>3);
                if (b >= vtcm_input->pcrSelection.sizeOfSelect) {
                    printf("Index out of range.\n");
                    exit(-1);
                }
                vtcm_input->pcrSelection.pcrSelect[b] |= (1 << ((index & 0x07)));
            } 
            else {
                printf("Missing parameter for -ix.\n");
                exit(-1);
            }
        } 
        i++;
    }
    vtcm_input->tag=htons(TCM_TAG_RQU_COMMAND);
    vtcm_input->ordinal=SUBTYPE_PCRRESET_IN;
    vtcm_template=memdb_get_template(DTYPE_VTCM_IN,SUBTYPE_PCRRESET_IN);
    if(vtcm_template==NULL){
        return -EINVAL;
    }
    vtcm_input->paramSize=sizeof(*vtcm_input);
    ret=struct_2_blob(vtcm_input,Buf,vtcm_template);
    if(ret<0)
        return ret;
    printf("Send message for pcrreset\n");
    print_bin_data(Buf,ret,8);
    ret=vtcmutils_transmit(vtcm_input->paramSize,Buf,&outlen,Buf);		
    if(ret<0)
        return ret;
    vtcm_template=memdb_get_template(DTYPE_VTCM_OUT,SUBTYPE_PCRREAD_OUT);
    if(vtcm_template==NULL)
        return -EINVAL;
    print_bin_data(Buf,outlen,8);
    return ret;
}
int proc_vtcmutils_PcrRead(void * sub_proc, void * para)
{
    int i = 1;
    int ret = 0;
    int index = -1;
    int outlen;

    struct tcm_in_pcrread * vtcm_input;
    struct tcm_out_pcrread * vtcm_output;
    void * vtcm_template;
    char * curr_para;

    unsigned char digest[TCM_HASH_SIZE];
    printf("begin proc pcrread \n");

    vtcm_input=Talloc0(sizeof(*vtcm_input));
    if(vtcm_input==NULL)
        return -ENOMEM;	
    vtcm_output=Talloc0(sizeof(*vtcm_output));
    if(vtcm_output==NULL)
        return -ENOMEM;	

    struct tcm_utils_input * input_para=para;

    while (i < input_para->param_num) {
        curr_para=input_para->params+i*DIGEST_SIZE;
        if (!strcmp("-ix",curr_para)) {
            i++;
            curr_para=input_para->params+i*DIGEST_SIZE;
            if (i <= input_para->param_num)
            {
                sscanf(curr_para,"%d",&vtcm_input->pcrIndex);
            } 
            else {
                printf("Missing parameter for -ix.\n");
                return -1;
            }
        } 
        else {
            printf("\n%s is not a valid option\n", curr_para);
            break;
        }
        i++;
    }
    vtcm_input->tag=htons(TCM_TAG_RQU_COMMAND);
    vtcm_input->ordinal=SUBTYPE_PCRREAD_IN;		
    vtcm_template=memdb_get_template(DTYPE_VTCM_IN,SUBTYPE_PCRREAD_IN);
    if(vtcm_template==NULL){
        return -EINVAL;
    }
    vtcm_input->paramSize=sizeof(*vtcm_input);
    ret=struct_2_blob(vtcm_input,Buf,vtcm_template);
    if(ret<0)
        return ret;
    print_bin_data(Buf,ret,8);
    ret=vtcmutils_transmit(vtcm_input->paramSize,Buf,&outlen,Buf);		
    if(ret<0)
        return ret;
    vtcm_template=memdb_get_template(DTYPE_VTCM_OUT,SUBTYPE_PCRREAD_OUT);
    if(vtcm_template==NULL)
        return -EINVAL;
    ret=blob_2_struct(Buf,vtcm_output,vtcm_template);
    if(ret<0)
        return ret;
    printf("Pcr %d value:\n",vtcm_input->pcrIndex);
    print_bin_data(vtcm_output->outDigest,32,8);
   /* i=0;
    while(i<TCM_HASH_SIZE){
        printf("%.2x ",vtcm_output->outDigest[i]);
        i++;
    }		

    printf("\n");*/
    return ret;
}

int vtcmutils_transmit(int in_len,BYTE * in, int * out_len, BYTE * out)
{
    int ret;
    int sockfd,sock_dt;
    struct sockaddr_in my_addr;//local ip info
    struct sockaddr_in dest_addr; //destnation ip info

    char * tcm_socket_name;
    int tcm_port;
    char * temp_str;
    tcm_socket_name=getenv("TCM_SERVER_NAME");
    if(tcm_socket_name==NULL)
        return -EINVAL;
    temp_str=getenv("TCM_SERVER_PORT");
    if(temp_str==NULL)
        return -EINVAL;
    tcm_port=Atoi(temp_str,DIGEST_SIZE);	

    if(-1 == (sockfd = socket(AF_INET,SOCK_STREAM,0)) )
    {
        print_cubeerr("error in create socket\n");
        return -1;
    }
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(tcm_port);
    dest_addr.sin_addr.s_addr = inet_addr(tcm_socket_name);
    memset(&dest_addr.sin_zero,0,8);
    if(-1 == connect(sockfd,(struct sockaddr*)&dest_addr,sizeof(struct sockaddr)))
    {
        print_cubeerr("connect error\n");
        return -EINVAL;
    }
    ret = send(sockfd,in,in_len,0);
    if(ret!=in_len)
        return -EINVAL;
    print_cubeaudit("write %d data!\n",ret);
    ret=recv(sockfd,out,1024,0);
    print_cubeaudit("read %d data!\n",ret);
    close(sockfd);
    *out_len=ret;
    return ret;
}

int proc_vtcmutils_MakeIdentity(void * sub_proc, void * para)
{
    int outlen;
    int i=2;
    int ret=0;
    int fd;
    void *vtcm_template;
    unsigned char nonce[TCM_HASH_SIZE];
    struct tcm_utils_input * input_para=para;
    char * index_para;
    char * value_para;

    struct tcm_in_MakeIdentity * vtcm_input;
    struct tcm_out_MakeIdentity * vtcm_output;
    char * pwdo="ooo";
    char * pwds="sss";
    char * pwdk="kkk";
    BYTE ownerauth[TCM_HASH_SIZE];
    BYTE smkauth[TCM_HASH_SIZE];
    BYTE pikauth[TCM_HASH_SIZE];
    BYTE cmdHash[TCM_HASH_SIZE];
    int  enclen=512;
    TCM_SM2_ASYMKEY_PARAMETERS * pik_parms;
    TCM_SESSION_DATA * ownerauthdata;
    TCM_SESSION_DATA * smkauthdata;

    // makeidentity's param
    TCM_AUTHHANDLE ownerhandle;		
    TCM_AUTHHANDLE smkhandle;		
    char * userinfo=NULL;
    char * reqfile=NULL;		
    char * keyfile=NULL;		

    vtcm_input = Talloc0(sizeof(*vtcm_input));
    if(vtcm_input==NULL)
        return -ENOMEM;
    vtcm_output = Talloc0(sizeof(*vtcm_output));
    if(vtcm_output==NULL)
        return -ENOMEM;
    vtcm_input->tag = htons(TCM_TAG_RQU_AUTH2_COMMAND);
    vtcm_input->ordinal = SUBTYPE_MAKEIDENTITY_IN;
    int offset=0;

    // get makeidentity's params 
	if((input_para->param_num>0)&&
		(input_para->param_num%2==1))
	{
		for(i=1;i<input_para->param_num;i+=2)
		{
        		index_para=input_para->params+i*DIGEST_SIZE;
        		value_para=index_para+DIGEST_SIZE;
			if(!Strcmp("-ish",index_para))
			{
                		sscanf(value_para,"%x",&vtcm_input->smkHandle);
			}	
			else if(!Strcmp("-ioh",index_para))
			{
                		sscanf(value_para,"%x",&vtcm_input->ownerHandle);
			}	
			else if(!Strcmp("-if",index_para))
			{
				userinfo=value_para;
			}
			else if(!Strcmp("-of",index_para))
			{
				reqfile=value_para;
			}
			else if(!Strcmp("-kf",index_para))
			{
				keyfile=value_para;
			}
			else
			{
				printf("Error cmd format! should be %s -ism smkhandle -ioh ownerhandle -if userfile -of reqfile",
					input_para->params);
				return -EINVAL;
			}
		}
	}	
	
    // find the ownerauthsession and smkauthsession 

    ownerauthdata = Find_AuthSession(TCM_ET_OWNER,vtcm_input->ownerHandle);
    if(ownerauthdata==NULL)
    {
        printf("can't find owner session for makeidentity!\n");
        return -EINVAL;
    }	

    smkauthdata = Find_AuthSession(TCM_ET_SMK,vtcm_input->smkHandle);
    if(smkauthdata==NULL)
    {
        printf("can't find smk session for makeidentity!\n");
        return -EINVAL;
    }	

    // compute the three auth value

    sm3(pwdo,Strlen(pwdo),ownerauth);
    sm3(pwds,Strlen(pwds),smkauth);
    sm3(pwdk,Strlen(pwdk),pikauth);

    // compute crypt pik auth
   for(i=0;i<TCM_HASH_SIZE;i++)
   {
 	vtcm_input->pikAuth[i]=pikauth[i]^ownerauthdata->sharedSecret[i];
   } 
  
    // compute pubDigest

    fd=open(userinfo,O_RDONLY);
    if(fd<0)
    {
  	printf("No userinfo file %s!\n",userinfo);
	return -EINVAL;
    }
    
    ret=read(fd,Buf,DIGEST_SIZE*64+1);
    if(ret<0)
    {
	printf("can't read userinfo data!\n");
	return -EINVAL;
    }
    if(ret>DIGEST_SIZE*64)
    {
	printf("user info too long!\n");
	return -EINVAL;
    }
    if(CApubkey==NULL)
    {
	printf("can't find CA's public key!\n");
	return -EINVAL;
    }
    Memcpy(Buf+ret,CApubkey,64);
    sm3(Buf,ret+64,vtcm_input->pubDigest); 	
    
    //  add vtcm_input's pikParams

    vtcm_input->pikParams.tag=htons(TCM_TAG_KEY);
    vtcm_input->pikParams.keyUsage=TCM_SM2KEY_IDENTITY;
    vtcm_input->pikParams.keyFlags=0;
//  vtcm_input->pikParams.migratable=FALSE;
    vtcm_input->pikParams.authDataUsage=TCM_AUTH_ALWAYS;
    vtcm_input->pikParams.algorithmParms.algorithmID=TCM_ALG_SM2;
    vtcm_input->pikParams.algorithmParms.encScheme=TCM_ES_NONE;
    vtcm_input->pikParams.algorithmParms.sigScheme=TCM_SS_SM2;


    vtcm_input->pikParams.PCRInfoSize=0;
    vtcm_input->pikParams.PCRInfo=NULL;
    vtcm_input->pikParams.pubKey.keyLength=0;
    vtcm_input->pikParams.pubKey.key=NULL;
    vtcm_input->pikParams.encDataSize=0;
    vtcm_input->pikParams.encData=NULL;

    // add pikparms's sm2 key parms
    pik_parms=Talloc0(sizeof(*pik_parms));
    if(pik_parms==NULL)
        return -ENOMEM;
    pik_parms->keyLength=0x80;
    vtcm_template=memdb_get_template(DTYPE_VTCM_IN_KEY,SUBTYPE_TCM_BIN_SM2_ASYMKEY_PARAMETERS);
    if(vtcm_template==NULL)
        return -EINVAL;

    ret=struct_2_blob(pik_parms,Buf,vtcm_template);
    if(ret<0)
        return ret;	

    vtcm_input->pikParams.algorithmParms.parmSize=ret;
    vtcm_input->pikParams.algorithmParms.parms=Talloc0(ret);
    if(vtcm_input->pikParams.algorithmParms.parms==NULL)
        return -ENOMEM;
    Memcpy(vtcm_input->pikParams.algorithmParms.parms,Buf,ret);

    // output command's bin value 

    vtcm_template=memdb_get_template(DTYPE_VTCM_IN,SUBTYPE_MAKEIDENTITY_IN);
    if(vtcm_template==NULL)
        return -EINVAL;
    offset = struct_2_blob(vtcm_input,Buf,vtcm_template);
    if(offset<0)
        return offset;

    vtcm_input->paramSize=offset;

    uint32_t temp_int;
    // compute smkauthCode
    sm3(Buf+6,offset-6-36*2,smkauth);

    Memcpy(Buf,smkauth,DIGEST_SIZE);
    temp_int=htonl(vtcm_input->smkHandle);
    Memcpy(Buf+DIGEST_SIZE,&temp_int,sizeof(uint32_t));
    
    sm3_hmac(smkauthdata->sharedSecret,TCM_HASH_SIZE,
	Buf,DIGEST_SIZE+sizeof(uint32_t),
	vtcm_input->smkAuth);

    // compute ownerauthCode
    Memcpy(Buf,smkauth,DIGEST_SIZE);
    temp_int=htonl(vtcm_input->ownerHandle);
    Memcpy(Buf+DIGEST_SIZE,&temp_int,sizeof(uint32_t));
    
    sm3_hmac(ownerauthdata->sharedSecret,TCM_HASH_SIZE,
	Buf,DIGEST_SIZE+sizeof(uint32_t),
	vtcm_input->ownerAuth);

    printf("Begin input for makeidentity:\n");
    offset = struct_2_blob(vtcm_input,Buf,vtcm_template);
    if(offset<0)
        return offset;

    print_bin_data(Buf,offset,8);
    ret = vtcmutils_transmit(vtcm_input->paramSize,Buf,&outlen,Buf);
    if(ret<0)
        return ret;
    printf("makeidentity:\n");
    print_bin_data(Buf,outlen,8);

    vtcm_template=memdb_get_template(DTYPE_VTCM_OUT,SUBTYPE_MAKEIDENTITY_OUT);
    if(vtcm_template==NULL)
	return -EINVAL;
    	
    ret=blob_2_struct(Buf,vtcm_output,vtcm_template);
    if(ret<0)
	return -EINVAL;
    	
    vtcm_template=memdb_get_template(DTYPE_VTCM_IN_KEY,SUBTYPE_TCM_BIN_KEY);
    if(vtcm_template==NULL)
	return -EINVAL;	
 
    // write keyfile	

    ret=struct_2_blob(&vtcm_output->pik,Buf,vtcm_template);
    if(ret<0)
	return -EINVAL;
    fd=open(keyfile,O_CREAT|O_TRUNC|O_WRONLY,0666);
    if(fd<0)
	return -EIO;
    write(fd,Buf,ret);
    close(fd);

    // write req file
    fd=open(reqfile,O_CREAT|O_TRUNC|O_WRONLY,0666);
    if(fd<0)
	return -EIO;
    write(fd,vtcm_output->CertData,vtcm_output->CertSize);
    close(fd);
			
    return ret;
}

int proc_vtcmutils_Quote(void * sub_proc, void * para)
{
    int outlen;
    int i=2;
    int ret=0;
    int fd;
    void *vtcm_template;
    unsigned char nonce[TCM_HASH_SIZE];
    struct tcm_utils_input * input_para=para;
    char * index_para;
    char * value_para;
    int pcrIndex;
    char * infofile;

    struct tcm_in_Quote * vtcm_input;
    struct tcm_out_Quote * vtcm_output;
    char * pwdo="ooo";
    char * pwds="sss";
    char * pwdk="kkk";

    BYTE externalData[TCM_HASH_SIZE];
    BYTE pikauth[TCM_HASH_SIZE];
    BYTE cmdHash[TCM_HASH_SIZE];

    int  enclen=512;
    TCM_SM2_ASYMKEY_PARAMETERS * pik_parms;

    TCM_SESSION_DATA * pikauthdata;

    // makeidentity's param
    TCM_AUTHHANDLE pikhandle;		

    char * userinfo=NULL;
    char * reqfile=NULL;		
    char * keyfile=NULL;		

    vtcm_input = Talloc0(sizeof(*vtcm_input));
    if(vtcm_input==NULL)
        return -ENOMEM;
    vtcm_output = Talloc0(sizeof(*vtcm_output));
    if(vtcm_output==NULL)
        return -ENOMEM;
    vtcm_input->tag = htons(TCM_TAG_RQU_AUTH1_COMMAND);
    vtcm_input->ordinal = SUBTYPE_QUOTE_IN;
    int offset=0;

    // get makeidentity's params 
	if((input_para->param_num>0)&&
		(input_para->param_num%2==1))
	{
		for(i=1;i<input_para->param_num;i+=2)
		{
        		index_para=input_para->params+i*DIGEST_SIZE;
        		value_para=index_para+DIGEST_SIZE;
			if(!Strcmp("-ikh",index_para))
			{
                		sscanf(value_para,"%x",&vtcm_input->keyHandle);
			}	
			else if(!Strcmp("-ish",index_para))
			{
                		sscanf(value_para,"%x",&vtcm_input->authHandle);
			}	
			else if(!Strcmp("-ix",index_para))
			{
				sscanf(value_para,"%d",&pcrIndex);
			}
			else if(!Strcmp("-of",index_para))
			{
				infofile=value_para;
			}
			else
			{
				printf("Error cmd format! should be %s -ikh keyhandle -ish authhandle -ix pcrindex -of infofile",
					input_para->params);
				return -EINVAL;
			}
		}
	}	
	
    // find the pikauthsession  

    pikauthdata = Find_AuthSession(TCM_ET_KEYHANDLE,vtcm_input->authHandle);
    if(pikauthdata==NULL)
    {
        printf("can't find pik session for quote!\n");
        return -EINVAL;
    }	


    // compute the pik auth value

    sm3(pwdk,Strlen(pwdk),pikauth);

    // compute crypt pik auth
    vtcm_AuthSessionData_Encrypt(vtcm_input->privAuth,pikauthdata,pikauth);

    Memset(vtcm_input->externalData,'\02',DIGEST_SIZE);	


    // file pcr_select struct
    vtcm_Init_PcrSelection(&vtcm_input->targetPCR);
    ret=vtcm_Set_PcrSelection(&vtcm_input->targetPCR,pcrIndex);
    if(ret<0)
    {
	printf("Invalid pcrIndex!\n");
	return -EINVAL;
    }

    // output command's bin value 

    vtcm_template=memdb_get_template(DTYPE_VTCM_IN,SUBTYPE_QUOTE_IN);
    if(vtcm_template==NULL)
        return -EINVAL;
    offset = struct_2_blob(vtcm_input,Buf,vtcm_template);
    if(offset<0)
        return offset;

    vtcm_input->paramSize=offset;

    uint32_t temp_int;
    // compute smkauthCode
    sm3(Buf+6,offset-6-36,pikauth);

    Memcpy(Buf,pikauth,DIGEST_SIZE);
    temp_int=htonl(vtcm_input->authHandle);
    Memcpy(Buf+DIGEST_SIZE,&temp_int,sizeof(uint32_t));
    
    sm3_hmac(pikauthdata->sharedSecret,TCM_HASH_SIZE,
	Buf,DIGEST_SIZE+sizeof(uint32_t),
	vtcm_input->privAuth);

    printf("Begin input for quote:\n");
    offset = struct_2_blob(vtcm_input,Buf,vtcm_template);
    if(offset<0)
        return offset;

    print_bin_data(Buf,offset,8);
    ret = vtcmutils_transmit(vtcm_input->paramSize,Buf,&outlen,Buf);
    if(ret<0)
        return ret;
    printf("quote:\n");
    print_bin_data(Buf,outlen,8);

    vtcm_template=memdb_get_template(DTYPE_VTCM_OUT,SUBTYPE_QUOTE_OUT);
    if(vtcm_template==NULL)
	return -EINVAL;
    	
    ret=blob_2_struct(Buf,vtcm_output,vtcm_template);
    if(ret<0)
	return -EINVAL;
    /*	
    vtcm_template=memdb_get_template(DTYPE_VTCM_IN_KEY,SUBTYPE_TCM_BIN_KEY);
    if(vtcm_template==NULL)
	return -EINVAL;	
 
    // write keyfile	

    ret=struct_2_blob(&vtcm_output->pik,Buf,vtcm_template);
    if(ret<0)
	return -EINVAL;
    fd=open(keyfile,O_CREAT|O_TRUNC|O_WRONLY,0666);
    if(fd<0)
	return -EIO;
    write(fd,Buf,ret);
    close(fd);

    // write req file
    fd=open(reqfile,O_CREAT|O_TRUNC|O_WRONLY,0666);
    if(fd<0)
	return -EIO;
    write(fd,vtcm_output->CertData,vtcm_output->CertSize);
    close(fd);
*/			
    return ret;
}