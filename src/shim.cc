/*
 * Copyright Joyent, Inc. and other Node contributors.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to permit
 * persons to whom the Software is furnished to do so, subject to the
 * following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN
 * NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
 * OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
 * USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include <cstdarg>
#include <cstdlib>
#include <cstring>

#include "shim.h"
#include "uv.h"
#include "shim-impl.h"

#if NODE_VERSION_AT_LEAST(0, 11, 3)
#define V8_USE_UNSAFE_HANDLES 1
#define V8_ALLOW_ACCESS_TO_RAW_HANDLE_CONSTRUCTOR 1
#endif

#include "v8.h"
#include "node.h"
#include "node_buffer.h"

namespace shim {

#if NODE_VERSION_AT_LEAST(0, 11, 3)
using v8::FunctionCallbackInfo;
#else
using node::Buffer;
using v8::Arguments;
#endif

using v8::Array;
using v8::Exception;
using v8::External;
using v8::Function;
using v8::FunctionTemplate;
using v8::Handle;
using v8::HandleScope;
using v8::Integer;
using v8::Isolate;
using v8::Local;
using v8::Number;
using v8::Null;
using v8::Object;
using v8::Persistent;
using v8::String;
using v8::TryCatch;
using v8::Undefined;
using v8::Value;


#define SHIM_PROLOGUE(ctx)                                                    \
  Isolate* ctx ## _isolate = Isolate::GetCurrent();                           \
  HandleScope ctx ## _scope;                                                  \
  TryCatch ctx ## _trycatch;                                                  \
  shim_ctx_t ctx;                                                             \
  ctx.isolate = static_cast<void*>(ctx ## _isolate);                          \
  ctx.scope = static_cast<void*>(&ctx ## _scope);                             \
  ctx.trycatch = static_cast<void*>(&ctx ## _trycatch);                       \
do {} while(0)


#define SHIM_TO_VAL(obj) Local<Value>(static_cast<Value*>((obj)->handle))

#define VAL_DEFINE(var, obj) Local<Value> var(SHIM_TO_VAL(obj))

#if 0
#define SHIM_DEBUG(...) fprintf(stderr, __VA_ARGS__)
#else
#define SHIM_DEBUG(...)
#endif

Persistent<String> hidden_private;

shim_val_t shim__undefined = {
  NULL,
  SHIM_TYPE_UNDEFINED,
};

shim_val_t shim__null = {
  NULL,
  SHIM_TYPE_NULL,
};

void
shim_context_cleanup(shim_ctx_t* ctx)
{
}


shim_val_t*
shim_val_alloc(shim_ctx_t* ctx, Handle<Value> val,
  shim_type_t type = SHIM_TYPE_UNKNOWN)
{
  shim_val_t* obj = static_cast<shim_val_t*>(malloc(sizeof(shim_val_t)));
  obj->handle = *val;
  obj->type = type;
  return obj;
}

Local<Value>*
shim_vals_to_handles(size_t argc, shim_val_t** argv)
{
  Local<Value>* jsargs = new Local<Value>[argc];

  for (size_t i = 0; i < argc; i++) {
    if (argv[i] == NULL) {
      jsargs[i] = *Null();
      continue;
    }

    Local<Value> tmp;

    switch(argv[i]->type) {
      case SHIM_TYPE_UNDEFINED:
        tmp = *Undefined();
        break;
      case SHIM_TYPE_NULL:
        tmp = *Null();
        break;
      default:
        tmp = SHIM_TO_VAL(argv[i]);
        break;
    }

    jsargs[i] = tmp;
  }

  return jsargs;
}


enum shim_err_type {
  SHIM_ERR_ERROR,
  SHIM_ERR_TYPE,
  SHIM_ERR_RANGE,
};

/* TODO this doesn't belong here */
#define SHIM_ERROR_LENGTH 512

Local<Value>
shim_format_error(shim_ctx_t* ctx, enum shim_err_type type, const char* msg,
  va_list ap)
{
  char buf[SHIM_ERROR_LENGTH];
  vsnprintf(buf, SHIM_ERROR_LENGTH, msg, ap);
  Local<String> str = String::New(buf);
  Local<Value> err;
  switch(type) {
    case SHIM_ERR_ERROR:
      err = Exception::Error(str);
      break;
    case SHIM_ERR_TYPE:
      err = Exception::TypeError(str);
      break;
    case SHIM_ERR_RANGE:
      err = Exception::RangeError(str);
      break;
  }
  return err;
}


Local<Value>
shim_call_func(Local<Object> recv, Local<Function> fn, size_t argc,
  shim_val_t** argv)
{
  Local<Value>* jsargs = shim_vals_to_handles(argc, argv);
  Local<Value> ret = fn->Call(recv, argc, jsargs);
  delete jsargs;
  return ret;
}


Local<Value>
shim_call_func(Local<Object> recv, Local<String> str, size_t argc,
  shim_val_t** argv)
{
  Local<Value> fh = recv->Get(str);
  assert(fh->IsFunction());
  Local<Function> fn = fh.As<Function>();
  return shim_call_func(recv, fn, argc, argv);
}


struct shim_fholder_s {
  shim_func cfunc;
  void* data;
};


#if NODE_VERSION_AT_LEAST(0, 11, 3)
void
Static(const FunctionCallbackInfo<Value>& args)
#else
Handle<Value>
Static(const Arguments& args)
#endif
{
  String::Utf8Value fname(args.Callee()->GetName());
  SHIM_DEBUG("SHIM ENTER %s\n", *fname);
  SHIM_PROLOGUE(ctx);

  Local<Value> data = args.Data();

  assert(data->IsExternal());

  Local<External> ext = data.As<External>();
  shim_fholder_s* holder = reinterpret_cast<shim_fholder_s*>(ext->Value());
  shim_func cfunc = holder->cfunc;

  shim_args_t sargs;
  sargs.argc = args.Length();
  sargs.argv = NULL;
  sargs.ret = shim_undefined();
  sargs.self = shim_val_alloc(&ctx, args.This());
  sargs.data = holder->data;

  size_t argv_len = sizeof(shim_val_t*) * sargs.argc;

  if (argv_len > 0)
    sargs.argv = static_cast<shim_val_t**>(malloc(argv_len));

  size_t i;

  for (i = 0; i < sargs.argc; i++) {
    sargs.argv[i] = shim_val_alloc(&ctx, args[i]);
  }

  SHIM_DEBUG("SHIM CALL %s\n", *fname);
  if(!cfunc(&ctx, &sargs)) {
    SHIM_DEBUG("SHIM ERROR %s\n", *fname);
    /* the function failed do we need to do any more checking of exceptions? */
  }
  SHIM_DEBUG("SHIM EXIT %s\n", *fname);

  Value* ret = NULL;

  if(sargs.ret != NULL) {
    switch(sargs.ret->type) {
      case SHIM_TYPE_UNDEFINED:
        ret = *Undefined();
        break;
      case SHIM_TYPE_NULL:
        ret = *Null();
        break;
      default:
        ret = static_cast<Value*>(sargs.ret->handle);
        break;
    }
  } else {
    ret = *Null();
  }

  assert(ret != NULL);

  for (i = 0; i < sargs.argc; i++) {
    shim_value_release(sargs.argv[i]);
  }

  shim_value_release(sargs.self);

  if (argv_len > 0)
    free(sargs.argv);

  shim_context_cleanup(&ctx);

  /* TODO sometimes things don't always propogate? */
  if (ctx_trycatch.HasCaught()) {
    SHIM_DEBUG("SHIM THREW %s\n", *fname);
    ctx_trycatch.ReThrow();
  }

  SHIM_DEBUG("SHIM LEAVING %s\n", *fname);
#if NODE_VERSION_AT_LEAST(0, 11, 3)
  if (!ctx_trycatch.HasCaught())
    args.GetReturnValue().Set(Local<Value>(ret));
#else
  if (ctx_trycatch.HasCaught()) {
    return ctx_trycatch.Exception();
  } else {
    return ctx_scope.Close(Local<Value>(ret));
  }
#endif
}

extern "C"
{

void shim_module_initialize(Handle<Object> exports, Handle<Value> module)
{
  SHIM_PROLOGUE(ctx);

  if (hidden_private.IsEmpty()) {
    Handle<String> str = String::NewSymbol("shim_private");
#if NODE_VERSION_AT_LEAST(0, 11, 3)
    hidden_private = Persistent<String>::New(ctx_isolate, str);
#else
    hidden_private = Persistent<String>::New(str);
#endif
  }

  shim_val_t sexport;
  sexport.handle = *exports;

  shim_val_t smodule;
  smodule.handle = *module;

  if (!shim_initialize(&ctx, &sexport, &smodule)) {
    if (!ctx_trycatch.HasCaught())
      shim_throw_error(&ctx, "Failed to initialize module");
  }

  shim_context_cleanup(&ctx);
}


/* TODO abstract out so we don't need multiple temporaries */

/**
 * \param val The value to check
 * \param type The expected type
 * \return TRUE or FALSE
 *
 * This doesn't coerce types, merely checks that the value is or isn't of the
 * desired type
 */
shim_bool_t
shim_value_is(shim_val_t* val, shim_type_t type)
{
  if (val->type == type)
    return TRUE;

  VAL_DEFINE(obj, val);
  shim_bool_t ret = FALSE;

  switch (type)
  {
    case SHIM_TYPE_OBJECT:
      ret = obj->IsObject();
      break;
    case SHIM_TYPE_STRING:
      ret = obj->IsString();
      break;
    case SHIM_TYPE_NUMBER:
      ret = obj->IsNumber();
      break;
    case SHIM_TYPE_INTEGER:
      /* TODO */
      ret = obj->IsNumber();
      break;
    case SHIM_TYPE_INT32:
      ret = obj->IsInt32();
      break;
    case SHIM_TYPE_UINT32:
      ret = obj->IsUint32();
      break;
    case SHIM_TYPE_ARRAY:
      ret = obj->IsArray();
      break;
    case SHIM_TYPE_BOOL:
      ret = obj->IsBoolean();
      break;
    case SHIM_TYPE_UNDEFINED:
      ret = obj->IsUndefined();
      break;
    case SHIM_TYPE_NULL:
      ret = obj->IsNull();
      break;
    case SHIM_TYPE_EXTERNAL:
      ret = obj->IsExternal();
      break;
    case SHIM_TYPE_DATE:
      ret = obj->IsDate();
      break;
    case SHIM_TYPE_FUNCTION:
      ret = obj->IsFunction();
      break;
    case SHIM_TYPE_BUFFER:
      ret = Buffer::HasInstance(obj);
      break;
    case SHIM_TYPE_UNKNOWN:
    default:
      ret = FALSE;
      break;
  }

  if (ret)
    val->type = type;

  return ret;
}


#define OBJ_TO_ARRAY(obj) \
  ((obj)->IsArray() ? (obj).As<Array>() : Local<Array>::Cast(obj))

#define OBJ_TO_OBJECT(obj) \
  ((obj)->IsObject() ? (obj).As<Object>() : (obj)->ToObject())

#define OBJ_TO_STRING(obj) \
  ((obj)->IsObject() ? (obj).As<String>() : (obj)->ToString())

#define OBJ_TO_INT32(obj) \
  ((obj)->ToInt32())

#define OBJ_TO_UINT32(obj) \
  ((obj)->ToUint32())

#define OBJ_TO_NUMBER(obj) \
  ((obj)->IsNumber() ? (obj).As<Number>() : (obj)->ToNumber())

#define OBJ_TO_EXTERNAL(obj) \
  ((obj)->IsExternal() ? (obj).As<External>() : Local<External>::Cast(obj))

#define OBJ_TO_FUNCTION(obj) \
  ((obj)->IsFunction() ? (obj).As<Function>() : Local<Function>::Cast(obj))

/**
 * \param ctx The currently executed context
 * \param val The given value
 * \param type The desired type
 * \param rval The destination value
 * \return TRUE or FALSE
 *
 * Attempts to coerce the given value into another type, if it fails to do so
 * returns FALSE.
 */
shim_bool_t
shim_value_to(shim_ctx_t* ctx, shim_val_t* val, shim_type_t type,
  shim_val_t* rval)
{
  if (val->type == type) {
    rval->type = type;
    rval->handle = val->handle;
    return TRUE;
  }

  VAL_DEFINE(obj, val);

  switch (type) {
    case SHIM_TYPE_UNDEFINED:
      rval->handle = *Undefined();
      break;
    case SHIM_TYPE_NULL:
      rval->handle = *Null();
      break;
    case SHIM_TYPE_BOOL:
      rval->handle = *obj->ToBoolean();
      break;
    case SHIM_TYPE_ARRAY:
      rval->handle = *OBJ_TO_ARRAY(obj);
      break;
    case SHIM_TYPE_OBJECT:
      rval->handle = *OBJ_TO_OBJECT(obj);
      break;
    case SHIM_TYPE_INTEGER:
      /* TODO */
      rval->handle = *OBJ_TO_NUMBER(obj);
      break;
    case SHIM_TYPE_INT32:
      rval->handle = *OBJ_TO_INT32(obj);
      break;
    case SHIM_TYPE_UINT32:
      rval->handle = *OBJ_TO_UINT32(obj);
      break;
    case SHIM_TYPE_NUMBER:
      rval->handle = *OBJ_TO_NUMBER(obj);
      break;
    case SHIM_TYPE_EXTERNAL:
      rval->handle = *OBJ_TO_EXTERNAL(obj);
      break;
    case SHIM_TYPE_FUNCTION:
      rval->handle = *OBJ_TO_FUNCTION(obj);
      break;
    case SHIM_TYPE_STRING:
      rval->handle = *OBJ_TO_STRING(obj);
    case SHIM_TYPE_UNKNOWN:
    default:
      return FALSE;
  }

  rval->type = type;
  return TRUE;
}


shim_val_t*
shim_undefined()
{
  return &shim__undefined;
}


shim_val_t*
shim_null()
{
  return &shim__null;
}

/**
 * \param val The given value
 * \sa [memory](md_docs_memory.html)
 *
 * Presuming the value was not allocated for ::shim_args_t or being used for
 * shim_args_set_rval() use this method to free the allocated memory
 */
void
shim_value_release(shim_val_t* val)
{
  if (val != NULL && val->type != SHIM_TYPE_NULL
      && val->type != SHIM_TYPE_UNDEFINED)
    free(val);
}


/**
 * \param ctx The currently executing context
 * \param klass The constructor to be used (may be NULL)
 * \param proto The prototype that should be used (may be NULL)
 * \return A pointer to the created object
 * \sa shim_value_release()
 */
shim_val_t*
shim_obj_new(shim_ctx_t* ctx, shim_val_t* klass, shim_val_t* proto)
{
  /* TODO if klass != NULL we should FunctionTemplate::New() */
  Local<Object> obj = Object::New();

  if (proto != NULL) {
    VAL_DEFINE(jsproto, proto);
    obj->SetPrototype(jsproto->ToObject());
  }

  return shim_val_alloc(ctx, obj);
}


/**
 * \param ctx The currently executing context
 * \param klass The given constructor
 * \param argc The amount of arguments being passed
 * \param argv The array of arguments to be passed
 * \return The returned object
 *
 * Use this to instantiate an object with the given arguments
 */
shim_val_t*
shim_obj_new_instance(shim_ctx_t* ctx, shim_val_t* klass, size_t argc,
  shim_val_t** argv)
{
  return NULL;
}

/**
 * \param ctx The currently executing context
 * \param src The given value
 * \return The new Local value representing the previous value
 *
 * Use this when you might want to create a Local of a persistent
 * \sa persistents
 */
shim_val_t*
shim_obj_clone(shim_ctx_t* ctx, shim_val_t* src)
{
  Local<Value> dst = Local<Value>::New(SHIM_TO_VAL(src));
  return shim_val_alloc(ctx, dst);
}

/**
 * \param ctx The currently executing context
 * \param val The given object
 * \param name The name of the property
 * \return TRUE if the object has the named property, otherwise FALSE
 *
 * Has the name
 */
shim_bool_t
shim_obj_has_name(shim_ctx_t* ctx, shim_val_t* val, const char* name)
{
  Local<Object> obj = OBJ_TO_OBJECT(SHIM_TO_VAL(val));
  return obj->Has(String::NewSymbol(name)) ? TRUE : FALSE;
}

/**
 * \param ctx The currently executing context
 * \param val The given object
 * \param id The id of the property
 * \return TRUE if the object has the indexed property, otherwise FALSE
 *
 * Object can have ordered properties, or may be an array, use this to check
 * if either exist.
 */
shim_bool_t
shim_obj_has_id(shim_ctx_t* ctx, shim_val_t* val, uint32_t id)
{
  Local<Object> obj = OBJ_TO_OBJECT(SHIM_TO_VAL(val));
  return obj->Has(id) ? TRUE : FALSE;
}

/**
 * \param ctx The currently executing context
 * \param val The given object
 * \param sym The symbol of the property
 * \return TRUE if the object has the property, otherwise FALSE
 *
 * Use this if you are keeping a reference to a commonly used symbol name
 * or are passed a value to lookup
 */
shim_bool_t
shim_obj_has_sym(shim_ctx_t* ctx, shim_val_t* val, shim_val_t* sym)
{
  Local<Object> obj = OBJ_TO_OBJECT(SHIM_TO_VAL(val));
  return obj->Has(OBJ_TO_STRING(SHIM_TO_VAL(sym)));
}

/**
 * \param ctx The currently executing context
 * \param obj The given object
 * \param name The name of the property
 * \param val The value to be stored
 * \return TRUE if the property was set, otherwise FALSE
 */
shim_bool_t
shim_obj_set_prop_name(shim_ctx_t* ctx, shim_val_t* obj, const char* name,
  shim_val_t* val)
{
  Local<Object> jsobj = OBJ_TO_OBJECT(SHIM_TO_VAL(obj));
  return jsobj->Set(String::NewSymbol(name), SHIM_TO_VAL(val));
}


/**
 * \param ctx The currently executing context
 * \param obj The given object
 * \param id The id of the property
 * \param val The value to be stored
 * \return TRUE if the property was set, otherwise FALSE
 */
shim_bool_t
shim_obj_set_prop_id(shim_ctx_t* ctx, shim_val_t* obj, uint32_t id,
  shim_val_t* val)
{
  Local<Object> jsobj = OBJ_TO_OBJECT(SHIM_TO_VAL(obj));
  return jsobj->Set(id, SHIM_TO_VAL(val));
}


/**
 * \param ctx The currently executing context
 * \param obj The given object
 * \param sym The symbol of the property
 * \param val The value to be stored
 * \return TRUE if the property was set, otherwise FALSE
 */
shim_bool_t
shim_obj_set_prop_sym(shim_ctx_t* ctx, shim_val_t* obj, shim_val_t* sym,
  shim_val_t* val)
{
  Local<Object> jsobj = OBJ_TO_OBJECT(SHIM_TO_VAL(obj));
  return jsobj->Set(SHIM_TO_VAL(sym), SHIM_TO_VAL(val));
}

/**
 * \param ctx The currently executing context
 * \param obj The given object
 * \param data The data to be associated with the object
 * \return TRUE if the data was able to be associated, otherwise FALSE
 *
 * Use this to associate C memory with a given object, which can be recalled
 * at a later time with shim_obj_get_private()
 *
 * \sa shim_obj_make_weak()
 */
shim_bool_t
shim_obj_set_private(shim_ctx_t* ctx, shim_val_t* obj, void* data)
{
  Local<Object> jsobj = OBJ_TO_OBJECT(SHIM_TO_VAL(obj));
  return jsobj->SetHiddenValue(hidden_private, External::New(data));
}

/**
 * \param ctx The currently executing context
 * \param recv The object to add the functions to
 * \param funcs The null terminated array of functions
 * \return TRUE if all functions were able to be added, otherwise FALSE
 */
shim_bool_t
shim_obj_set_funcs(shim_ctx_t* ctx, shim_val_t* recv,
  const shim_fspec_t* funcs)
{
  size_t i = 0;
  shim_fspec_t cur = funcs[i];
  while (cur.name != NULL) {
    shim_val_t* func = shim_func_new(ctx, cur.cfunc, cur.nargs, cur.flags,
      cur.name, cur.data);

    if (!shim::shim_obj_set_prop_name(ctx, recv, cur.name, func))
      return FALSE;

    shim::shim_value_release(func);
    cur = funcs[++i];
  }

  return TRUE;
}

/**
 * \param ctx The currently executing context
 * \param obj The the given object
 * \param name The name of the property
 * \param rval The actual value returned
 * \return TRUE if object had the propert, otherwise FALSE
 */
shim_bool_t
shim_obj_get_prop_name(shim_ctx_t* ctx, shim_val_t* obj, const char* name,
  shim_val_t* rval)
{
  Local<Object> jsobj = OBJ_TO_OBJECT(SHIM_TO_VAL(obj));
  Local<Value> val = jsobj->Get(String::NewSymbol(name));
  rval->handle = *val;
  rval->type = SHIM_TYPE_UNKNOWN;
  return TRUE;
}


/**
 * \param ctx The currently executing context
 * \param obj The the given object
 * \param idx The id of the property
 * \param rval The actual value returned
 * \return TRUE if object had the propert, otherwise FALSE
 */
shim_bool_t
shim_obj_get_prop_id(shim_ctx_t* ctx, shim_val_t* obj, uint32_t idx,
  shim_val_t* rval)
{
  Local<Object> jsobj = OBJ_TO_OBJECT(SHIM_TO_VAL(obj));
  Local<Value> val = jsobj->Get(idx);
  rval->handle = *val;
  rval->type = SHIM_TYPE_UNKNOWN;
  return TRUE;
}


/**
 * \param ctx The currently executing context
 * \param obj The the given object
 * \param sym The symbol of the property
 * \param rval The actual value returned
 * \return TRUE if object had the propert, otherwise FALSE
 */
shim_bool_t
shim_obj_get_prop_sym(shim_ctx_t* ctx, shim_val_t* obj, shim_val_t* sym,
  shim_val_t* rval)
{
  Local<Object> jsobj = OBJ_TO_OBJECT(SHIM_TO_VAL(obj));
  Local<Value> val = jsobj->Get(SHIM_TO_VAL(sym));
  rval->handle = *val;
  rval->type = SHIM_TYPE_UNKNOWN;
  return TRUE;
}


/**
 * \param ctx The currently executing context
 * \param obj The the given object
 * \param data The actual data
 * \return TRUE if the object had hidden data, otherwise FALSE
 */
shim_bool_t
shim_obj_get_private(shim_ctx_t* ctx, shim_val_t* obj, void** data)
{
  Local<Object> jsobj = OBJ_TO_OBJECT(SHIM_TO_VAL(obj));
  Local<Value> ext = jsobj->GetHiddenValue(hidden_private);
  *data = ext.As<External>()->Value();
  return TRUE;
}

/**
 * \param ctx The currently executing context
 * \param val The the given object
 * \return The created persistent
 */
shim_val_t*
shim_persistent_new(shim_ctx_t* ctx, shim_val_t* val)
{
  VAL_DEFINE(obj, val);
  Persistent<Value> pobj = Persistent<Value>::New(
#if NODE_VERSION_AT_LEAST(0, 11, 3)
    static_cast<Isolate*>(ctx->isolate),
#endif
    obj);
  /* TODO flag a val as persistent */
  return shim_val_alloc(ctx, pobj);
}

/**
 * \param val The persistent to dispose
 */
void
shim_persistent_dispose(shim_val_t* val)
{
  Persistent<Value> tmp(SHIM_TO_VAL(val));
  tmp.Dispose();
  free(val);
}


void
#if NODE_VERSION_AT_LEAST(0, 11, 3)
common_weak_cb(Isolate* iso, Persistent<Value>* pobj, weak_baton_t* baton)
{
  Persistent<Value> obj = *pobj;
#else
common_weak_cb(Persistent<Value> obj, void* data)
{
  weak_baton_t* baton = static_cast<weak_baton_t*>(data);
#endif
  SHIM_PROLOGUE(ctx);
  shim_val_t* tmp = shim_val_alloc(NULL, obj);
  baton->weak_cb(tmp, baton->data);
  delete baton;
}

/**
 * \param ctx Currently executing context
 * \param val Persistent to make weak
 * \param data Arbitrary data to pass to the weak_cb
 * \param weak_cb Callback that will be called when the object is about to be
 * free
 */
void
shim_obj_make_weak(shim_ctx_t* ctx, shim_val_t* val, void* data,
  shim_weak_cb weak_cb)
{
  /* TODO check that this is not a persistent? */
  Persistent<Value> tmp(SHIM_TO_VAL(val));

  weak_baton_t *baton = new weak_baton_t;
  baton->weak_cb = weak_cb;
  baton->data = data;

  tmp.MakeWeak(baton, common_weak_cb);
}

/**
 * \param val The given persistent
 */
void
shim_obj_clear_weak(shim_val_t* val)
{
  Persistent<Value> tmp(SHIM_TO_VAL(val));
  tmp.ClearWeak();
}

/**
 * \param ctx Currently executing context
 * \param cfunc The function pointer to be executed
 * \param argc The number of arguments the function takes
 * \param flags The flags for the function
 * \param name The name of the function
 * \param hint Arbitrary data to keep associated with the function
 * \return The wrapped function
 */
shim_val_t*
shim_func_new(shim_ctx_t* ctx, shim_func cfunc, size_t argc, int32_t flags,
  const char* name, void* hint)
{
  shim_fholder_s* holder = new shim_fholder_s;
  holder->cfunc = cfunc;
  holder->data = hint;

  Local<External> ext = External::New(reinterpret_cast<void*>(holder));

  Local<FunctionTemplate> ft = FunctionTemplate::New(shim::Static, ext);
  Local<Function> fh = ft->GetFunction();
  fh->SetName(String::NewSymbol(name));
  return shim_val_alloc(ctx, fh);
}

/**
 * \param ctx Currently executing context
 * \param self The this parameter of the function call
 * \param sym The symbol representing the name of the function
 * \param argc The number of args to pass the function
 * \param argv The array of arguments to pass to the function
 * \param rval The return value of the function
 * \return TRUE if the function succeeded, otherwise FALSE
 */
shim_bool_t
shim_func_call_sym(shim_ctx_t* ctx, shim_val_t* self, shim_val_t* sym,
  size_t argc, shim_val_t** argv, shim_val_t* rval)
{
  assert(self != NULL);
  Local<Object> recv = OBJ_TO_OBJECT(SHIM_TO_VAL(self));

  Local<String> str = OBJ_TO_STRING(SHIM_TO_VAL(sym));

  rval->handle = *shim_call_func(recv, str, argc, argv);

  TryCatch *tr = static_cast<TryCatch*>(ctx->trycatch);
  return !tr->HasCaught();
}

/**
 * \param ctx Currently executing context
 * \param self The this parameter of the function call
 * \param name The name of the function
 * \param argc The number of args to pass the function
 * \param argv The array of arguments to pass to the function
 * \param rval The return value of the function
 * \return TRUE if the function succeeded, otherwise FALSE
 */
shim_bool_t
shim_func_call_name(shim_ctx_t* ctx, shim_val_t* self, const char* name,
  size_t argc, shim_val_t** argv, shim_val_t* rval)
{
  assert(self != NULL);
  Local<Object> recv = OBJ_TO_OBJECT(SHIM_TO_VAL(self));

  rval->handle = *shim_call_func(recv, String::NewSymbol(name), argc, argv);
  
  TryCatch *tr = static_cast<TryCatch*>(ctx->trycatch);
  return !tr->HasCaught();
}

/**
 * \param ctx Currently executing context
 * \param self The this parameter of the function call
 * \param func The function to be called
 * \param argc The number of args to pass the function
 * \param argv The array of arguments to pass to the function
 * \param rval The return value of the function
 * \return TRUE if the function succeeded, otherwise FALSE
 */
shim_bool_t
shim_func_call_val(shim_ctx_t* ctx, shim_val_t* self, shim_val_t* func,
  size_t argc, shim_val_t** argv, shim_val_t* rval)
{
  Local<Value> fh = SHIM_TO_VAL(func);
  assert(fh->IsFunction());
  Local<Function> fn = fh.As<Function>();

  Local<Object> recv;

  if (self != NULL)
    recv = OBJ_TO_OBJECT(SHIM_TO_VAL(self));
  else
    recv = Object::New();

  rval->handle = *shim_call_func(recv, fn, argc, argv);

  TryCatch *tr = static_cast<TryCatch*>(ctx->trycatch);
  return !tr->HasCaught();
}

/**
 * \param ctx Currently executing context
 * \param self The this parameter of the function call
 * \param sym The symbol representing the name of the function
 * \param argc The number of args to pass the function
 * \param argv The array of arguments to pass to the function
 * \param rval The return value of the function
 * \return TRUE if the function succeeded, otherwise FALSE
 */
shim_bool_t
shim_make_callback_sym(shim_ctx_t* ctx, shim_val_t* self, shim_val_t* sym,
  size_t argc, shim_val_t** argv, shim_val_t* rval)
{
  assert(self != NULL);

  Local<Object> recv = OBJ_TO_OBJECT(SHIM_TO_VAL(self));
  Local<String> jsym = OBJ_TO_STRING(SHIM_TO_VAL(sym));

  Local<Value>* jsargs = shim_vals_to_handles(argc, argv);

  rval->handle = *node::MakeCallback(recv, jsym, argc, jsargs);

  delete jsargs;

  TryCatch *tr = static_cast<TryCatch*>(ctx->trycatch);
  return !tr->HasCaught();
}

/**
 * \param ctx Currently executing context
 * \param self The this parameter of the function call
 * \param fval The function to call
 * \param argc The number of args to pass the function
 * \param argv The array of arguments to pass to the function
 * \param rval The return value of the function
 * \return TRUE if the function succeeded, otherwise FALSE
 */
shim_bool_t
shim_make_callback_val(shim_ctx_t* ctx, shim_val_t* self, shim_val_t* fval,
  size_t argc, shim_val_t** argv, shim_val_t* rval)
{
  /* TODO check is valid */
  Local<Value> prop = SHIM_TO_VAL(fval);
  Local<Function> fn = Local<Function>::Cast(prop);
  Local<Value>* jsargs = shim_vals_to_handles(argc, argv);

  Local<Object> recv;

  if (self != NULL) {
    recv = OBJ_TO_OBJECT(SHIM_TO_VAL(self));
  } else {
    recv = Object::New();
  }

  Handle<Value> ret = node::MakeCallback(recv, fn, argc, jsargs);

  rval->handle = *ret;

  delete jsargs;

  TryCatch *tr = static_cast<TryCatch*>(ctx->trycatch);
  return !tr->HasCaught();
}

/**
 * \param ctx Currently executing context
 * \param obj The this parameter of the function call
 * \param name The name of the function
 * \param argc The number of args to pass the function
 * \param argv The array of arguments to pass to the function
 * \param rval The return value of the function
 * \return TRUE if the function succeeded, otherwise FALSE
 */
shim_bool_t
shim_make_callback_name(shim_ctx_t* ctx, shim_val_t* obj, const char* name,
  size_t argc, shim_val_t** argv, shim_val_t* rval)
{
  assert(obj != NULL);

  Local<Value>* jsargs = shim_vals_to_handles(argc, argv);
  Local<Object> recv = OBJ_TO_OBJECT(SHIM_TO_VAL(obj));

  Handle<Value> ret = node::MakeCallback(recv, name, argc, jsargs);

  rval->handle = *ret;

  delete jsargs;

  TryCatch *tr = static_cast<TryCatch*>(ctx->trycatch);
  return !tr->HasCaught();
}

/**
 * \param ctx Current executing context
 * \param d The value of the new number
 * \return The wrapped number
 */
shim_val_t*
shim_number_new(shim_ctx_t* ctx, double d)
{
  return shim_val_alloc(ctx, Number::New(d));
}

/**
 * \param val The given number
 * \return The value of the number
 */
double
shim_number_value(shim_val_t* val)
{
  return SHIM_TO_VAL(val)->NumberValue();
}

/**
 * \param ctx Current executing context
 * \param i The value of the new integer
 * \return The wrapped integer
 */
shim_val_t*
shim_integer_new(shim_ctx_t* ctx, int32_t i)
{
  return shim_val_alloc(ctx, Integer::New(i));
}

/**
 * \param ctx Current executing context
 * \param i The value of the new integer
 * \return The wrapped integer
 */
shim_val_t*
shim_integer_uint(shim_ctx_t* ctx, uint32_t i)
{
  return shim_val_alloc(ctx, Integer::NewFromUnsigned(i));
}

/**
 * \param val The given integer
 * \return The value of the integer
 */
int64_t
shim_integer_value(shim_val_t* val)
{
  return SHIM_TO_VAL(val)->IntegerValue();
}

/**
 * \param val The given integer
 * \return The int32_t value of the integer
 */
int32_t
shim_integer_int32_value(shim_val_t* val)
{
  return SHIM_TO_VAL(val)->Int32Value();
}

/**
 * \param val The given integer
 * \return The uint32_t value of the integer
 */
uint32_t
shim_integer_uint32_value(shim_val_t* val)
{
  return SHIM_TO_VAL(val)->Uint32Value();
}

/**
 * \param ctx Current executing context
 * \return Wrapped empty string
 */
shim_val_t*
shim_string_new(shim_ctx_t* ctx)
{
  return shim_val_alloc(ctx, String::Empty());
}

/**
 * \param ctx Current executing context
 * \param data Source null terminated string
 * \return The wrapped string
 */
shim_val_t*
shim_string_new_copy(shim_ctx_t* ctx, const char* data)
{
  return shim_val_alloc(ctx, String::New(data));
}

/**
 * \param ctx Current executing context
 * \param data Source string
 * \param len Length of created string
 * \return The wrapped string
 */
shim_val_t*
shim_string_new_copyn(shim_ctx_t* ctx, const char* data, size_t len)
{
  return shim_val_alloc(ctx, String::New(data, len));
}

/**
 * \param val The given string
 * \return The length of the string
 */
size_t
shim_string_length(shim_val_t* val)
{
  return OBJ_TO_STRING(SHIM_TO_VAL(val))->Length();
}

/**
 * \param val The given string
 * \return The length of the UTF-8 encoded string
 */
size_t
shim_string_length_utf8(shim_val_t* val)
{
  return OBJ_TO_STRING(SHIM_TO_VAL(val))->Utf8Length();
}

/**
 * \param val The given string
 * \return The C string value of the string
 *
 * The caller is responsible for free'ing this memory
 */
const char*
shim_string_value(shim_val_t* val)
{
  String::Utf8Value str(OBJ_TO_STRING(SHIM_TO_VAL(val)));
  return strdup(*str);
}

/**
 * \param val The given string
 * \param buff The destination buffer
 * \param start The starting position to encode
 * \param len The length of the string to create
 * \param options The options for how to encode the string
 */
size_t
shim_string_write_ascii(shim_val_t* val, char* buff, size_t start, size_t len,
  int32_t options)
{
  return OBJ_TO_STRING(SHIM_TO_VAL(val))->WriteAscii(buff, start, len);
}

/**
 * \param ctx Current executing context
 * \param len Length of array to be created
 * \return Wrapped array
 */
shim_val_t*
shim_array_new(shim_ctx_t* ctx, size_t len)
{
  return shim_val_alloc(ctx, Array::New(len));
}

/**
 * \param arr Given array
 * \return Length of the array
 */
size_t
shim_array_length(shim_val_t* arr)
{
  return OBJ_TO_ARRAY(SHIM_TO_VAL(arr))->Length();
}

/**
 * \param ctx Current executing context
 * \param arr Given array
 * \param idx Index of element
 * \param rval Actual return value
 * \return TRUE if the value existed, otherwise FALSE
 */
shim_bool_t
shim_array_get(shim_ctx_t* ctx, shim_val_t* arr, int32_t idx, shim_val_t* rval)
{
  rval->handle = *OBJ_TO_ARRAY(SHIM_TO_VAL(arr))->Get(idx);
  return TRUE;
}

/**
 * \param ctx Current executing context
 * \param arr Given array
 * \param idx Index of element
 * \param val Value to be set
 * \return TRUE if the value was able to be set, otherwise FALSE
 */
shim_bool_t
shim_array_set(shim_ctx_t* ctx, shim_val_t* arr, int32_t idx, shim_val_t* val)
{
  return OBJ_TO_ARRAY(SHIM_TO_VAL(arr))->Set(idx, SHIM_TO_VAL(val));
}

/**
 * \param ctx Current executing context
 * \param len Size of buffer to create
 * \return Wrapped buffer
 */
shim_val_t*
shim_buffer_new(shim_ctx_t* ctx, size_t len)
{
#if NODE_VERSION_AT_LEAST(0, 11, 3)
  return shim_val_alloc(ctx, node::Buffer::New(len));
#else
  return shim_val_alloc(ctx, Buffer::New(len)->handle_);
#endif
}

/**
 * \param ctx Current executing context
 * \param data Data to be copied
 * \param len Length of data to be copied
 * \return Wrapped buffer
 */
shim_val_t*
shim_buffer_new_copy(shim_ctx_t* ctx, const char* data, size_t len)
{
#if NODE_VERSION_AT_LEAST(0, 11, 3)
  return shim_val_alloc(ctx, node::Buffer::New(data, len));
#elif NODE_VERSION_AT_LEAST(0, 10, 0)
  return shim_val_alloc(ctx, Buffer::New(data, len)->handle_);
#else
  return shim_val_alloc(ctx, Buffer::New((char*)data, len)->handle_);
#endif
}

/**
 * \param ctx Current executing context
 * \param data Data to be used for underlying memory
 * \param len Size of memory being used
 * \param cb Callback that is called when buffer is to be freed
 * \param hint Arbitrary data passed to the callback
 * \return Wrapped buffer
 *
 * The underlying memory is not copied, but used in place
 */
shim_val_t*
shim_buffer_new_external(shim_ctx_t* ctx, char* data, size_t len,
  shim_buffer_free cb, void* hint)
{
#if NODE_VERSION_AT_LEAST(0, 11, 3)
  return shim_val_alloc(ctx, node::Buffer::New(data, len, cb, hint));
#else
  return shim_val_alloc(ctx, Buffer::New(data, len, cb, hint)->handle_);
#endif
}

/**
 * \param val THe given buffer
 * \return Pointer to the underlying memory
 */
char*
shim_buffer_value(shim_val_t* val)
{
  Local<Value>v(SHIM_TO_VAL(val));
#if NODE_VERSION_AT_LEAST(0, 11, 3)
  assert(node::Buffer::HasInstance(v));
  return node::Buffer::Data(v);
#elif NODE_VERSION_AT_LEAST(0, 10, 0)
  assert(Buffer::HasInstance(v));
  return Buffer::Data(v);
#else
  assert(Buffer::HasInstance(v));
  return Buffer::Data(v.As<Object>());
#endif
}

/**
 * \param val The given buffer
 * \return THe size of the buffer
 */
size_t
shim_buffer_length(shim_val_t* val)
{
  Local<Value>v(SHIM_TO_VAL(val));
#if NODE_VERSION_AT_LEAST(0, 11, 3)
  assert(node::Buffer::HasInstance(v));
  return node::Buffer::Length(v);
#elif NODE_VERSION_AT_LEAST(0, 10, 0)
  assert(Buffer::HasInstance(v));
  return Buffer::Length(v);
#else
  assert(Buffer::HasInstance(v));
  return Buffer::Length(v.As<Object>());
#endif
}

/**
 * \param ctx Currently executing context
 * \param data The external data to wrap
 * \return Wrapped external
 *
 * This is an object that is safe to pass to and from JavaScript.
 * \sa persistents
 */
shim_val_t*
shim_external_new(shim_ctx_t* ctx, void* data)
{
  Local<Value> e;

#if NODE_VERSION_AT_LEAST(0, 11, 3)
  e = External::New(data);
#else
  e = External::Wrap(data);
#endif

  return shim_val_alloc(ctx, e);
}

/**
 * \param ctx Currently executing context
 * \param obj The given external
 * \return The pointer to the wrapped memory
 */
void*
shim_external_value(shim_ctx_t* ctx, shim_val_t* obj)
{
  void* ret;
  Local<External> e = SHIM_TO_VAL(obj).As<External>();

#if NODE_VERSION_AT_LEAST(0, 11, 3)
  ret = e->Value();
#else
  ret = External::Unwrap(e);
#endif

  return ret;
}

/**
 * \param ctx Currently executing context
 * \param msg The message to be used for error
 * \param ... Arguments for formatting
 * \return The wrapped Error
 */
shim_val_t*
shim_error_new(shim_ctx_t* ctx, const char* msg, ...)
{
  va_list ap;
  va_start(ap, msg);
  Local<Value> err = shim::shim_format_error(ctx, SHIM_ERR_ERROR, msg, ap);
  va_end(ap);
  return shim_val_alloc(ctx, err);
}

/**
 * \param ctx Currently executing context
 * \param msg The message to be used for error
 * \param ... Arguments for formatting
 * \return The wrapped TypeError
 */
shim_val_t*
shim_error_type_new(shim_ctx_t* ctx, const char* msg, ...)
{
  va_list ap;
  va_start(ap, msg);
  Local<Value> err = shim::shim_format_error(ctx, SHIM_ERR_TYPE, msg, ap);
  va_end(ap);
  return shim_val_alloc(ctx, err);
}

/**
 * \param ctx Currently executing context
 * \param msg The message to be used for error
 * \param ... Arguments for formatting
 * \return The wrapped RangeError
 */
shim_val_t*
shim_error_range_new(shim_ctx_t* ctx, const char* msg, ...)
{
  va_list ap;
  va_start(ap, msg);
  Local<Value> err = shim::shim_format_error(ctx, SHIM_ERR_RANGE, msg, ap);
  va_end(ap);
  return shim_val_alloc(ctx, err);
}

/**
 * \param ctx Currently executing context
 * \return TRUE if an exception is pending, otherwise FALSE
 */
shim_bool_t
shim_exception_pending(shim_ctx_t* ctx)
{
  TryCatch* trycatch = static_cast<TryCatch*>(ctx->trycatch);
  return trycatch->HasCaught();
}

/**
 * \param ctx Currently executing context
 * \param val The error to use as the pending exception
 */
void
shim_exception_set(shim_ctx_t* ctx, shim_val_t* val)
{
  ThrowException(SHIM_TO_VAL(val));
}

/**
 * \param ctx Currently executing context
 * \param rval The currently pending exception
 * \return TRUE if there was an exception, otherwise FALSE
 */
shim_bool_t
shim_exception_get(shim_ctx_t* ctx, shim_val_t* rval)
{
  TryCatch* trycatch = static_cast<TryCatch*>(ctx->trycatch);
  rval->handle = *trycatch->Exception();
  return TRUE;
}

/**
 * \param ctx Currently executing context
 */
void
shim_exception_clear(shim_ctx_t* ctx)
{
  TryCatch* trycatch = static_cast<TryCatch*>(ctx->trycatch);
  trycatch->Reset();
}

/**
 * \param ctx Currently executing context
 * \param msg The message to set as the pending exception
 * \param ... Arguments for formatting
 */
void
shim_throw_error(shim_ctx_t* ctx, const char* msg, ...)
{
  va_list ap;
  va_start(ap, msg);
  ThrowException(shim::shim_format_error(ctx, SHIM_ERR_ERROR, msg, ap));
  va_end(ap);
}

/**
 * \param ctx Currently executing context
 * \param msg The message to set as the pending exception
 * \param ... Arguments for formatting
 */
void
shim_throw_type_error(shim_ctx_t* ctx, const char* msg, ...)
{
  va_list ap;
  va_start(ap, msg);
  ThrowException(shim::shim_format_error(ctx, SHIM_ERR_TYPE, msg, ap));
  va_end(ap);
}

/**
 * \param ctx Currently executing context
 * \param msg The message to set as the pending exception
 * \param ... Arguments for formatting
 */
void
shim_throw_range_error(shim_ctx_t* ctx, const char* msg, ...)
{
  va_list ap;
  va_start(ap, msg);
  ThrowException(shim::shim_format_error(ctx, SHIM_ERR_RANGE, msg, ap));
  va_end(ap);
}

/**
 * \param ctx Currently executing context
 * \param arg Given wrapped value
 * \param type The destination type
 * \param rval The pointer to where the data will be stored
 * \return TRUE if it was able to convert, otherwise FALSE
 */
shim_bool_t
shim_unpack_type(shim_ctx_t* ctx, shim_val_t* arg, shim_type_t type,
  void* rval)
{
  if (!shim::shim_value_is(arg, type))
    return FALSE;

  Local<Value> val(SHIM_TO_VAL(arg));
  shim_val_t* vrval = *(shim_val_t**)rval;
  switch(type) {
    case SHIM_TYPE_BOOL:
      *(shim_bool_t*)rval = val->BooleanValue();
      break;
    case SHIM_TYPE_INTEGER:
      *(int64_t*)rval = val->IntegerValue();
      break;
    case SHIM_TYPE_UINT32:
      *(uint32_t*)rval = val->Uint32Value();
      break;
    case SHIM_TYPE_INT32:
      *(int32_t*)rval = val->Int32Value();
      break;
    case SHIM_TYPE_NUMBER:
      *(double*)rval = val->NumberValue();
      break;
    case SHIM_TYPE_EXTERNAL:
      *(void**)rval = shim::shim_external_value(ctx, arg);
      break;
    case SHIM_TYPE_BUFFER:
      *(char**)rval = shim::shim_buffer_value(arg);
      break;
    case SHIM_TYPE_STRING:
      vrval->handle = *OBJ_TO_STRING(val);
      break;
    case SHIM_TYPE_UNDEFINED:
    case SHIM_TYPE_NULL:
    case SHIM_TYPE_DATE:
    case SHIM_TYPE_ARRAY:
    case SHIM_TYPE_OBJECT:
    case SHIM_TYPE_FUNCTION:
    default:
      return FALSE;
      break;
  }

  return TRUE;
}

/**
 * \param ctx Currently executing context
 * \param args Arguments passed to the function
 * \param idx Index of the argument to unpack
 * \param type The destination type
 * \param rval The pointer to the destination
 * \return TRUE if it was able to unpack, otherwise FALSE
 */
shim_bool_t
shim_unpack_one(shim_ctx_t* ctx, shim_args_t* args, uint32_t idx,
  shim_type_t type, void* rval)
{
  shim_val_t* arg = args->argv[idx];
  return shim::shim_unpack_type(ctx, arg, type, rval);
}

/**
 * \param ctx Currently executing context
 * \param args Arguments passed to the function
 * \param type The first type to unpack
 * \return TRUE if all desired arguments were able to be unpacked, otherwise
 * FALSE
 *
 * Arguments are passed in pairs of shim_type_t and a pointer to their
 * destination, you end the set of pairs by a single SHIM_TYPE_UNKNOWN.
 *
 * In the event an argument was unable to be unpacked, FALSE is returned and
 * an exception is set.
 */
shim_bool_t
shim_unpack(shim_ctx_t* ctx, shim_args_t* args, shim_type_t type, ...)
{
  size_t cur;
  shim_type_t ctype = type;
  va_list ap;

  va_start(ap, type);

  for (cur = 0, ctype = type; ctype != SHIM_TYPE_UNKNOWN && cur < args->argc; cur++)
  {
    void* rval = va_arg(ap, void*);

    if(!shim::shim_unpack_one(ctx, args, cur, ctype, rval)) {
      /* TODO this should use a type string */
      shim::shim_throw_type_error(ctx, "Argument %d not of type %s", cur,
        shim_type_str(ctype));
      return FALSE;
    }

    ctype = static_cast<shim_type_t>(va_arg(ap, int));
  }

  va_end(ap);

  return TRUE;
}

/**
 * \param args The arguments passed to the function
 * \return The amount of arguments passed to the function
 */
size_t
shim_args_length(shim_args_t* args)
{
  return args->argc;
}

/**
 * \param args The arguments passed to the function
 * \param idx The index of the desired argument
 * \return The wrapped argument
 */
shim_val_t*
shim_args_get(shim_args_t* args, size_t idx)
{
  /* TODO assert */
  return args->argv[idx];
}

/**
 * \param ctx Currently executing context
 * \param args The arguments passed to the function
 * \param val The value to use for return
 * \return TRUE if it was able to set the return value, otherwise FALSE
 */
shim_bool_t
shim_args_set_rval(shim_ctx_t* ctx, shim_args_t* args, shim_val_t* val)
{
  /* TODO check if persistent */
  /*
  if (!shim_has_allocd(ctx, val))
    allocs_from_ctx(ctx)->insert(val);
  */
  args->ret = val;
  return TRUE;
}

/**
 * \param ctx Currently executing context
 * \param args The arguments passed to the function
 * \return The `this` associated with the executing function
 */
shim_val_t*
shim_args_get_this(shim_ctx_t* ctx, shim_args_t* args)
{
  return args->self;
}

/**
 * \param ctx Currently executing context
 * \param args The arguments passed to the function
 * \return Pointer to the arbitrary data associated with the executing function
 */
void*
shim_args_get_data(shim_ctx_t* ctx, shim_args_t* args)
{
  return args->data;
}


void
before_work(uv_work_t* req)
{
  shim_work_t* work = static_cast<shim_work_t*>(req->data);
  work->work_cb(work, work->hint);
}


void
#if NODE_VERSION_AT_LEAST(0, 10, 0)
before_after(uv_work_t* req, int status)
{
#else
before_after(uv_work_t* req)
{
  int status = 0;
#endif
  SHIM_PROLOGUE(ctx);
  shim_work_t* work = static_cast<shim_work_t*>(req->data);
  work->after_cb(&ctx, work, status, work->hint);
  shim_context_cleanup(&ctx);
  delete work;
  delete req;
}

/**
 * \param work_cb Callback that will be called on a different thread
 * \param after_cb Callback that will be called on the main thread
 * \param hint Arbitrary data to be passed to both callbacks
 */
void
shim_queue_work(shim_work_cb work_cb, shim_after_work after_cb, void* hint)
{
  uv_work_t* req = new uv_work_t;
  shim_work_t* work = new shim_work_t;
  work->work_cb = work_cb;
  work->after_cb = after_cb;
  work->hint = hint;
  req->data = work;
  uv_queue_work(uv_default_loop(), req, before_work, before_after);
}

/**
 * \param type The given type
 * \return The string representation of the given type
 */
const char*
shim_type_str(shim_type_t type)
{
  switch(type) {
#define SHIM_ITEM(type) \
    case type: \
      return #type;
      break;

    SHIM_ITEM(SHIM_TYPE_UNKNOWN)
    SHIM_ITEM(SHIM_TYPE_UNDEFINED)
    SHIM_ITEM(SHIM_TYPE_NULL)
    SHIM_ITEM(SHIM_TYPE_BOOL)
    SHIM_ITEM(SHIM_TYPE_DATE)
    SHIM_ITEM(SHIM_TYPE_ARRAY)
    SHIM_ITEM(SHIM_TYPE_OBJECT)
    SHIM_ITEM(SHIM_TYPE_INTEGER)
    SHIM_ITEM(SHIM_TYPE_INT32)
    SHIM_ITEM(SHIM_TYPE_UINT32)
    SHIM_ITEM(SHIM_TYPE_NUMBER)
    SHIM_ITEM(SHIM_TYPE_EXTERNAL)
    SHIM_ITEM(SHIM_TYPE_FUNCTION)
    SHIM_ITEM(SHIM_TYPE_STRING)
    SHIM_ITEM(SHIM_TYPE_BUFFER)
#undef SHIM_ITEM
  }
}


}

}
