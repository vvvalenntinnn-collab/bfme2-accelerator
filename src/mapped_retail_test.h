#pragma once
// Fixture image is prepared in Temp from the user's own executable. Reserve
// the preferred address in a suspended child before its CRT allocates heaps.
static int runMappedRetailChild(const char* path,bool benchmark) {
    FILE* f=fopen(path,"rb"); assert(f);
    DWORD header[3]; assert(fread(header,4,3,f)==3 && header[0]==0x58465752);
    char exe[MAX_PATH],command[MAX_PATH+32]; GetModuleFileNameA(NULL,exe,MAX_PATH);
    sprintf(command,"\"%s\" child%s",exe,benchmark?" benchmark":"");
    STARTUPINFOA startup={sizeof(startup)}; PROCESS_INFORMATION child={};
    assert(CreateProcessA(exe,command,NULL,NULL,TRUE,CREATE_SUSPENDED,NULL,NULL,&startup,&child));
    BYTE* image=(BYTE*)VirtualAllocEx(child.hProcess,(void*)0x400000,header[1],MEM_RESERVE|MEM_COMMIT,PAGE_EXECUTE_READWRITE);
    assert(image==(BYTE*)0x400000);
    for(DWORD i=0;i<header[2];++i) {
        DWORD section[2]; assert(fread(section,4,2,f)==2);
        assert(section[0]<=header[1] && section[1]<=header[1]-section[0]);
        std::vector<BYTE> bytes(section[1]); assert(fread(bytes.data(),1,section[1],f)==section[1]);
        SIZE_T written=0;
        assert(WriteProcessMemory(child.hProcess,image+section[0],bytes.data(),bytes.size(),&written) && written==bytes.size());
    }
    fclose(f); FlushInstructionCache(child.hProcess,image,header[1]);
    assert(ResumeThread(child.hThread)!=(DWORD)-1); CloseHandle(child.hThread);
    assert(WaitForSingleObject(child.hProcess,180000)==WAIT_OBJECT_0);
    DWORD code; assert(GetExitCodeProcess(child.hProcess,&code)); CloseHandle(child.hProcess); return (int)code;
}
