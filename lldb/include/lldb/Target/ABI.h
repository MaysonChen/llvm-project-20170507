//===-- ABI.h ---------------------------------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef liblldb_ABI_h_
#define liblldb_ABI_h_

// C Includes
// C++ Includes
// Other libraries and framework includes
// Project includes
#include "lldb/Core/Error.h"
#include "lldb/Core/PluginInterface.h"
#include "lldb/lldb-private.h"

namespace lldb_private {

class ABI :
    public PluginInterface
{
public:
    virtual
    ~ABI();

    virtual size_t
    GetRedZoneSize () const = 0;

    virtual bool
    PrepareTrivialCall (Thread &thread, 
                        lldb::addr_t sp,
                        lldb::addr_t functionAddress,
                        lldb::addr_t returnAddress, 
                        lldb::addr_t *arg1_ptr = NULL,
                        lldb::addr_t *arg2_ptr = NULL,
                        lldb::addr_t *arg3_ptr = NULL,
                        lldb::addr_t *arg4_ptr = NULL,
                        lldb::addr_t *arg5_ptr = NULL,
                        lldb::addr_t *arg6_ptr = NULL) const = 0;

    virtual bool
    GetArgumentValues (Thread &thread,
                       ValueList &values) const = 0;
    
public:
    lldb::ValueObjectSP
    GetReturnValueObject (Thread &thread,
                          ClangASTType &type,
                          bool persistent = true) const;
    
    // Set the Return value object in the current frame as though a function with 
    virtual Error
    SetReturnValueObject(lldb::StackFrameSP &frame_sp, lldb::ValueObjectSP &new_value) = 0;

protected:    
    // This is the method the ABI will call to actually calculate the return value.
    // Don't put it in a persistant value object, that will be done by the ABI::GetReturnValueObject.
    virtual lldb::ValueObjectSP
    GetReturnValueObjectImpl (Thread &thread,
                          ClangASTType &type) const = 0;
public:
    virtual bool
    CreateFunctionEntryUnwindPlan (UnwindPlan &unwind_plan) = 0;

    virtual bool
    CreateDefaultUnwindPlan (UnwindPlan &unwind_plan) = 0;

    virtual bool
    RegisterIsVolatile (const RegisterInfo *reg_info) = 0;

    virtual bool
    StackUsesFrames () = 0;

    virtual bool
    CallFrameAddressIsValid (lldb::addr_t cfa) = 0;

    virtual bool
    CodeAddressIsValid (lldb::addr_t pc) = 0;    

    virtual lldb::addr_t
    FixCodeAddress (lldb::addr_t pc)
    {
        // Some targets might use bits in a code address to indicate
        // a mode switch. ARM uses bit zero to signify a code address is
        // thumb, so any ARM ABI plug-ins would strip those bits.
        return pc;
    }

    virtual const RegisterInfo *
    GetRegisterInfoArray (uint32_t &count) = 0;

    
    
    bool
    GetRegisterInfoByName (const ConstString &name, RegisterInfo &info);

    bool
    GetRegisterInfoByKind (lldb::RegisterKind reg_kind, 
                           uint32_t reg_num, 
                           RegisterInfo &info);

    static lldb::ABISP
    FindPlugin (const ArchSpec &arch);
    
protected:
    //------------------------------------------------------------------
    // Classes that inherit from ABI can see and modify these
    //------------------------------------------------------------------
    ABI();
private:
    DISALLOW_COPY_AND_ASSIGN (ABI);
};

} // namespace lldb_private

#endif  // liblldb_ABI_h_
