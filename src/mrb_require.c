/*
** require.c - require
**
** See Copyright Notice in mruby.h
*/

#include "mruby.h"
#include "mruby/data.h"
#include "mruby/string.h"
#include "mruby/dump.h"
#include "mruby/proc.h"
#include "mruby/compile.h"
#include "mruby/variable.h"
#include "mruby/array.h"
#include "mruby/numeric.h"

#include "opcode.h"
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/param.h>
#include <limits.h>
#include <unistd.h>
#include <libgen.h>

#ifdef _WIN32
#include <windows.h>
#define dlopen(x,y) (void*)LoadLibrary(x)
#define dlsym(x,y) (void*)GetProcAddress((HMODULE)x,y)
#define dlclose(x) FreeLibrary((HMODULE)x)
const char* dlerror() {
  DWORD err = (int) GetLastError();
  if (err == 0) return NULL;
  static char buf[256];
  FormatMessage(
    FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
    NULL,
    err,
    MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
    buf,
    sizeof buf,
    NULL);
  return buf;
}

char*
realpath(const char *path, char *resolved_path) {
  if (!resolved_path)
    resolved_path = malloc(PATH_MAX + 1);
  if (!resolved_path) return NULL;
  GetFullPathNameA(path, PATH_MAX, resolved_path, NULL);
  return resolved_path;
}
#else
#include <dlfcn.h>
#endif

#if defined(_WIN32)
# define ENV_SEP ';'
# define SO_EXT ".dll"
# define environ _environ
#elif defined(__linux__)
# define ENV_SEP ':'
# define SO_EXT ".so"
#elif defined(__APPLE__)
# define ENV_SEP ':'
# define SO_EXT ".dyn"
#endif

#define E_LOAD_ERROR (mrb_class_obj_get(mrb, "ScriptError"))

#ifndef MAXPATHLEN
#define MAXPATHLEN 1024
#endif

#if 0
  #include <stdarg.h>
  #define debug(s,...) printf("%s:%d " s, __FILE__, __LINE__,__VA_ARGS__)
#else
  #define debug(...) ((void)0)
#endif

extern mrb_value mrb_file_exist(mrb_state *mrb, mrb_value fname);

mrb_value
mrb_yield_internal(mrb_state *mrb, mrb_value b, int argc, mrb_value *argv, mrb_value self, struct RClass *c);

static mrb_value
envpath_to_mrb_ary(mrb_state *mrb, const char *name)
{
  mrb_value ary = mrb_ary_new(mrb);

  char *env= getenv(name);
  if (env == NULL) {
    return ary;
  }

  int i;
  int envlen = strlen(env);
  for (i = 0; i < envlen; i++) {
    char *ptr = env + i;
    char *end = strchr(ptr, ENV_SEP);
    if (end == NULL) {
      end = env + envlen;
    }
    int len = end - ptr;
    mrb_ary_push(mrb, ary, mrb_str_new(mrb, ptr, len));
    i += len;
  }

  return ary;
}


static mrb_value
find_file_check(mrb_state *mrb, mrb_value path, mrb_value fname, mrb_value ext)
{
  mrb_value filepath = mrb_str_dup(mrb, path);
  mrb_str_cat2(mrb, filepath, "/");
  mrb_str_buf_append(mrb, filepath, fname);
  if (!mrb_nil_p(ext)) {
    mrb_str_buf_append(mrb, filepath, ext);
  }

  if (mrb_nil_p(filepath)) {
    return mrb_nil_value();
  }
  debug("filepath: %s\n", RSTRING_PTR(filepath));

  char fpath[MAXPATHLEN];
  realpath(RSTRING_PTR(filepath), fpath);
  if (fpath == NULL) {
    return mrb_nil_value();
  }
  debug("fpath: %s\n", fpath);

  FILE *fp = fopen(fpath, "r");
  if (fp == NULL) {
    return mrb_nil_value();
  }
  fclose(fp);

  return mrb_str_new2(mrb, fpath);
}

static mrb_value
find_file(mrb_state *mrb, mrb_value filename)
{
  char *fname = RSTRING_PTR(filename);
  mrb_value filepath = mrb_nil_value();
  mrb_value load_path = mrb_obj_dup(mrb, mrb_gv_get(mrb, mrb_intern(mrb, "$:")));
  load_path = mrb_check_array_type(mrb, load_path);

  if(mrb_nil_p(load_path)) {
    mrb_raise(mrb, E_RUNTIME_ERROR, "invalid $:");
    return mrb_undef_value();
  }

  char *ext = strrchr(fname, '.');
  mrb_value exts = mrb_ary_new(mrb);
  if (ext == NULL) {
    mrb_ary_push(mrb, exts, mrb_str_new2(mrb, ".rb"));
    mrb_ary_push(mrb, exts, mrb_str_new2(mrb, ".mrb"));
    mrb_ary_push(mrb, exts, mrb_str_new2(mrb, SO_EXT));
  } else {
    mrb_ary_push(mrb, exts, mrb_nil_value());
  }

  /* when absolute path */
  if (*fname == '/') {
    FILE *fp = fopen(fname, "r");
    if (fp == NULL) {
      return mrb_nil_value();
    }
    fclose(fp);
    return filename;
  }

  /* when a filename start with '.', $: = ['.'] */
  if (*fname == '.') {
    load_path = mrb_ary_new(mrb);
    mrb_ary_push(mrb, load_path, mrb_str_new2(mrb, "."));
  }

  int i, j;
  for (i = 0; i < RARRAY_LEN(load_path); i++) {
    for (j = 0; j < RARRAY_LEN(exts); j++) {
      filepath = find_file_check(mrb, RARRAY_PTR(load_path)[i], filename, RARRAY_PTR(exts)[j]);
      if (!mrb_nil_p(filepath)) {
        return filepath;
      }
    }
  }

  mrb_raisef(mrb, E_LOAD_ERROR, "cannot load such file -- %s", RSTRING_PTR(filename));
  return mrb_nil_value();
}

static void
replace_stop_with_return(mrb_state *mrb, mrb_irep *irep)
{
  if (irep->iseq[irep->ilen - 1] == MKOP_A(OP_STOP, 0)) {
    irep->iseq = mrb_realloc(mrb, irep->iseq, (irep->ilen + 1) * sizeof(mrb_code));
    irep->iseq[irep->ilen - 1] = MKOP_A(OP_LOADNIL, 0);
    irep->iseq[irep->ilen] = MKOP_AB(OP_RETURN, 0, OP_R_NORMAL);
    irep->ilen++;
  }
}

static void
load_mrb_file_with_filepath(mrb_state *mrb, mrb_value filepath, mrb_value origfilepath)
{
  char *fpath = RSTRING_PTR(filepath);

  {
    FILE *fp = fopen(fpath, "r");
    if (fp == NULL) {
      mrb_raisef(mrb, E_LOAD_ERROR, "can't load %s", fpath);
      return;
    }
    fclose(fp);
  }

  int sirep = mrb->irep_len;
  int arena_idx = mrb_gc_arena_save(mrb);

  FILE *fp = fopen(fpath, "r");
  int n = mrb_read_irep_file(mrb, fp);
  fclose(fp);

  mrb_gc_arena_restore(mrb, arena_idx);

  if (n >= 0) {
    struct RProc *proc;
    mrb_irep *irep = mrb->irep[n];
    int i;
    for (i = sirep; i < mrb->irep_len; i++) {
      mrb->irep[i]->filename = mrb_string_value_ptr(mrb, origfilepath);
    }

    replace_stop_with_return(mrb, irep);
    proc = mrb_proc_new(mrb, irep);
    proc->target_class = mrb->object_class;

    arena_idx = mrb_gc_arena_save(mrb);
    mrb_yield_internal(mrb, mrb_obj_value(proc), 0, NULL, mrb_top_self(mrb), mrb->object_class);
    mrb_gc_arena_restore(mrb, arena_idx);
  } else if (mrb->exc) {
    // fail to load
    longjmp(*(jmp_buf*)mrb->jmp, 1);
  }
}

static void
load_mrb_file(mrb_state *mrb, mrb_value filepath)
{
  load_mrb_file_with_filepath(mrb, filepath, filepath);
}

static mrb_value
mrb_compile(mrb_state *mrb0, char *tmpfilepath, char *filepath)
{
  mrb_state *mrb = mrb_open();
  mrbc_context *c;
  mrb_value result;
  FILE *fp;
  int irep_len = mrb->irep_len;

  debug("irep_len=%d\n", mrb->irep_len);
  fp = fopen(filepath, "r");
  c = mrbc_context_new(mrb);
  c->no_exec = 1;
  c->filename = filepath;
  result = mrb_load_file_cxt(mrb, fp, c);
  fclose(fp);
  debug("result=%d, irep_len=%d\n", mrb_fixnum(result), mrb->irep_len);

  fp = fopen(tmpfilepath, "w");
  mrb->irep += mrb_fixnum(result);
  debug("**dbg : %d\n", mrb->irep_len - irep_len);
  mrb->irep_len -= irep_len;
  mrb_dump_irep(mrb, 0, fp);
  fclose(fp);

  mrb->irep -= mrb_fixnum(result);
  mrb->irep_len += irep_len;
  mrb_close(mrb);

  return mrb_nil_value();
}

static void
load_so_file(mrb_state *mrb, mrb_value filepath)
{
  typedef void (*fn_mrb_gem_init)(mrb_state *mrb);
  void * handle = dlopen(RSTRING_PTR(filepath), RTLD_LAZY|RTLD_GLOBAL);
  if (!handle) {
    mrb_raise(mrb, E_RUNTIME_ERROR, dlerror());
  }
  dlerror(); // clear last error

  char entry[PATH_MAX] = {0};
  snprintf(entry, sizeof(entry)-1, "mrb_mruby_require_example_gem_init");

  fn_mrb_gem_init fn = (fn_mrb_gem_init) dlsym(handle, entry);
  if (fn == NULL) {
    mrb_raise(mrb, E_RUNTIME_ERROR, dlerror());
  }
  dlerror(); // clear last error

  fn(mrb);
}

static void
load_rb_file(mrb_state *mrb, mrb_value filepath)
{
  char *fpath = RSTRING_PTR(filepath);

  {
    FILE *fp = fopen(fpath, "r");
    if (fp == NULL) {
      mrb_raisef(mrb, E_LOAD_ERROR, "can't load %s", fpath);
      return;
    }
    fclose(fp);
  }

  mrb_value tmpfilepath;
#ifdef _WIN32
  char tmpfile[PATH_MAX];
  GetTempFileName(NULL, "mruby.", 0, tmpfile);
  tmpfilepath = mrb_str_new2(mrb, tmpfile);
#else
  mrb_value tmpfilepath = mrb_str_new2(mrb, "/tmp/mruby.");
#endif
  mrb_value pid = mrb_fixnum_value((int)getpid());
  mrb_str_buf_append(mrb, tmpfilepath, mrb_fix2str(mrb, pid, 10));
  debug("tmpfilepath: %s\n", RSTRING_PTR(tmpfilepath));

  mrb_compile(mrb, mrb_string_value_ptr(mrb, tmpfilepath),
      mrb_string_value_ptr(mrb, filepath));
  load_mrb_file_with_filepath(mrb, tmpfilepath, filepath);

  remove(RSTRING_PTR(tmpfilepath));
}

static void
load_file(mrb_state *mrb, mrb_value filepath)
{
  char *ext = strrchr(RSTRING_PTR(filepath), '.');
  if (ext == NULL) {
    mrb_raisef(mrb, E_LOAD_ERROR, "Filepath '%s' is invalid.", RSTRING_PTR(filepath));
    return ;
  }

  if (strcmp(ext, ".mrb") == 0) {
    load_mrb_file(mrb, filepath);
  } else if (strcmp(ext, ".rb") == 0) {
    load_rb_file(mrb, filepath);
  } else if (strcmp(ext, SO_EXT) == 0) {
    load_so_file(mrb, filepath);
  } else {
    mrb_raisef(mrb, E_LOAD_ERROR, "Filepath '%s' is invalid extension.",
        RSTRING_PTR(filepath));
    return;
  }
}

mrb_value
mrb_load(mrb_state *mrb, mrb_value filename)
{
  mrb_value filepath = find_file(mrb, filename);
  load_file(mrb, filepath);
  return mrb_true_value(); // TODO: ??
}

mrb_value
mrb_f_load(mrb_state *mrb, mrb_value self)
{
  mrb_value filename;

  mrb_get_args(mrb, "o", &filename);
  if (mrb_type(filename) != MRB_TT_STRING) {
    mrb_raisef(mrb, E_TYPE_ERROR, "can't convert %s into String",
        mrb_obj_classname(mrb, filename));
    return mrb_nil_value();
  }

  return mrb_load(mrb, filename);
}

static int
loaded_files_check(mrb_state *mrb, mrb_value filepath)
{
  mrb_value loaded_files = mrb_gv_get(mrb, mrb_intern(mrb, "$\""));
  int i;
  for (i = 0; i < RARRAY_LEN(loaded_files); i++) {
    if (mrb_str_cmp(mrb, RARRAY_PTR(loaded_files)[i], filepath) == 0) {
      return 0;
    }
  }

  mrb_value loading_files = mrb_gv_get(mrb, mrb_intern(mrb, "$\"_"));
  if (mrb_nil_p(loading_files)) {
    return 1;
  }
  for (i = 0; i < RARRAY_LEN(loading_files); i++) {
    if (mrb_str_cmp(mrb, RARRAY_PTR(loading_files)[i], filepath) == 0) {
      return 0;
    }
  }

  return 1;
}

static void
loading_files_add(mrb_state *mrb, mrb_value filepath)
{
  mrb_value loading_files = mrb_gv_get(mrb, mrb_intern(mrb, "$\"_"));
  if (mrb_nil_p(loading_files)) {
    loading_files = mrb_ary_new(mrb);
  }
  mrb_ary_push(mrb, loading_files, filepath);

  mrb_gv_set(mrb, mrb_intern(mrb, "$\"_"), loading_files);

  return;
}

static void
loaded_files_add(mrb_state *mrb, mrb_value filepath)
{
  mrb_value loaded_files = mrb_gv_get(mrb, mrb_intern(mrb, "$\""));
  mrb_ary_push(mrb, loaded_files, filepath);

  mrb_gv_set(mrb, mrb_intern(mrb, "$\""), loaded_files);

  return;
}

mrb_value
mrb_require(mrb_state *mrb, mrb_value filename)
{
  mrb_value filepath = find_file(mrb, filename);
  if (loaded_files_check(mrb, filepath)) {
    loading_files_add(mrb, filepath);
    load_file(mrb, filepath);
    loaded_files_add(mrb, filepath);
    return mrb_true_value();
  }

  return mrb_false_value();
}

mrb_value
mrb_f_require(mrb_state *mrb, mrb_value self)
{
  mrb_value filename;

  mrb_get_args(mrb, "o", &filename);
  if (mrb_type(filename) != MRB_TT_STRING) {
    mrb_raisef(mrb, E_TYPE_ERROR, "can't convert %s into String", mrb_obj_classname(mrb, filename));
    return mrb_nil_value();
  }

  return mrb_require(mrb, filename);
}

extern char **environ;

static mrb_value
mrb_init_load_path(mrb_state *mrb)
{
  return envpath_to_mrb_ary(mrb, "MRBLIB");
}

void
mrb_mruby_require_gem_init(mrb_state* mrb)
{
  struct RClass *krn;
  krn = mrb->kernel_module;

  mrb_define_method(mrb, krn, "load",    mrb_f_load,    ARGS_REQ(1));
  mrb_define_method(mrb, krn, "require", mrb_f_require, ARGS_REQ(1));

  mrb_gv_set(mrb, mrb_intern(mrb, "$:"), mrb_init_load_path(mrb));
  mrb_gv_set(mrb, mrb_intern(mrb, "$\""), mrb_ary_new(mrb));
}

void
mrb_mruby_require_gem_final(mrb_state* mrb)
{
}

/* vim:set et ts=2 sts=2 sw=2 tw=0: */
