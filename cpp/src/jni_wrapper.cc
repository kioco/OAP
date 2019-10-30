#include <arrow/buffer.h>
#include <arrow/io/memory.h>
#include <arrow/ipc/api.h>
#include <arrow/ipc/dictionary.h>
#include <jni.h>
#include <iostream>
#include <string>

#include "code_generator_factory.h"
#include "concurrent_map.h"
#include "gandiva/jni_common.h"

static jclass arrow_record_batch_builder_class;
static jmethodID arrow_record_batch_builder_constructor;

static jclass arrow_field_node_builder_class;
static jmethodID arrow_field_node_builder_constructor;

static jclass arrowbuf_builder_class;
static jmethodID arrowbuf_builder_constructor;

using arrow::jni::ConcurrentMap;
static ConcurrentMap<std::shared_ptr<arrow::Buffer>> buffer_holder_;

static jclass io_exception_class;
static jclass illegal_argument_exception_class;

static jint JNI_VERSION = JNI_VERSION_1_8;

#ifdef __cplusplus
extern "C" {
#endif

jint JNI_OnLoad(JavaVM* vm, void* reserved) {
  JNIEnv* env;
  if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION) != JNI_OK) {
    return JNI_ERR;
  }

  io_exception_class = CreateGlobalClassReference(env, "Ljava/io/IOException;");
  illegal_access_exception_class =
      CreateGlobalClassReference(env, "Ljava/lang/IllegalAccessException;");
  illegal_argument_exception_class =
      CreateGlobalClassReference(env, "Ljava/lang/IllegalArgumentException;");

  arrow_record_batch_builder_class = CreateGlobalClassReference(
      env, "Lorg/apache/arrow/adapter/parquet/ArrowRecordBatchBuilder;");
  arrow_record_batch_builder_constructor =
      GetMethodID(env, arrow_record_batch_builder_class, "<init>",
                  "(I[Lorg/apache/arrow/adapter/parquet/ArrowFieldNodeBuilder;"
                  "[Lorg/apache/arrow/adapter/parquet/ArrowBufBuilder;)V");

  arrow_field_node_builder_class = CreateGlobalClassReference(
      env, "Lorg/apache/arrow/adapter/parquet/ArrowFieldNodeBuilder;");
  arrow_field_node_builder_constructor =
      GetMethodID(env, arrow_field_node_builder_class, "<init>", "(II)V");

  arrowbuf_builder_class = CreateGlobalClassReference(
      env, "Lorg/apache/arrow/adapter/parquet/ArrowBufBuilder;");
  arrowbuf_builder_constructor =
      GetMethodID(env, arrowbuf_builder_class, "<init>", "(JJIJ)V");

  return JNI_VERSION;
}

void JNI_OnUnload(JavaVM* vm, void* reserved) {
  std::cerr << "JNI_OnUnload" << std::endl;
  JNIEnv* env;
  vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION);

  env->DeleteGlobalRef(io_exception_class);
  env->DeleteGlobalRef(illegal_access_exception_class);
  env->DeleteGlobalRef(illegal_argument_exception_class);

  env->DeleteGlobalRef(arrow_field_node_builder_class);
  env->DeleteGlobalRef(arrowbuf_builder_class);
  env->DeleteGlobalRef(arrow_record_batch_builder_class);

  buffer_holder_.Clear();
}

JNIEXPORT jlong JNICALL
Java_com_intel_sparkColumnarPlugin_vectorized_ExpressionEvaluator_nativeBuild(
    JNIEnv* env, jobject obj, jbyteArray schema_arr, jbyteArray exprs_arr) {
  arrow::Status status;

  jsize schema_len = env->GetArrayLength(schema_arr);
  jbyte* schema_bytes = env->GetByteArrayElements(schema_arr, 0);

  auto serialized_schema =
      std::make_shared<arrow::Buffer>((uint8_t*)schema_bytes, schema_len);
  arrow::ipc::DictionaryMemo in_memo;
  std::shared_ptr<arrow::Schema> schema;
  arrow::io::BufferReader buf_reader(serialized_schema);

  arrow::Status msg = arrow::ipc::ReadSchema(&buf_reader, &in_memo, &schema);
  if (!msg.ok()) {
    env->ReleaseByteArrayElements(schema_arr, schema_bytes, JNI_ABORT);
    std::string error_message =
        "nativeOpenParquetWriter: failed to readSchema, err msg is " + msg.message();
    env->ThrowNew(io_exception_class, error_message.c_str());
  }

  ExpressionVector expr_vector;
  FieldVector ret_types;
  types::ExpressionList exprs;
  jsize exprs_len = env->GetArrayLength(exprs_arr);
  jbyte* exprs_bytes = env->GetByteArrayElements(exprs_arr, 0);

  if (!ParseProtobuf(reinterpret_cast<uint8_t*>(exprs_bytes), exprs_len, &exprs)) {
    env->ReleaseByteArrayElements(schema_arr, schema_bytes, JNI_ABORT);
    env->ReleaseByteArrayElements(exprs_arr, exprs_bytes, JNI_ABORT);
    std::string error_message = "Unable to parse expressions protobuf";
    env->ThrowNew(io_exception_class, error_message.c_str());
  }

  // create Expression out of the list of exprs
  for (int i = 0; i < exprs.exprs_size(); i++) {
    ExpressionPtr root = ProtoTypeToExpression(exprs.exprs(i));

    if (root == nullptr) {
      env->ReleaseByteArrayElements(schema_arr, schema_bytes, JNI_ABORT);
      env->ReleaseByteArrayElements(exprs_arr, exprs_bytes, JNI_ABORT);
      std::string error_message =
          "Unable to construct expression object from expression protobuf";
      env->ThrowNew(io_exception_class, error_message.c_str());
    }

    expr_vector.push_back(root);
    ret_types.push_back(root->result());
  }

  CodeGenerator* handler;
  msg = CreateCodeGenerator(schema, expr_vector, ret_types, &handler);
  if (!msg.ok()) {
    std::string error_message =
        "nativeBuild: failed to create CodeGenerator, err msg is " + msg.message();
    env->ThrowNew(io_exception_class, error_message.c_str());
  }
  return (int64_t)handler;
}

JNIEXPORT void JNICALL
Java_com_intel_sparkColumnarPlugin_vectorized_ExpressionEvaluator_nativeClose(
    JNIEnv* env, jobject obj, jlong handler_ptr) {
  delete (CodeGenerator*)handler_ptr;
}

JNIEXPORT jobject JNICALL
Java_com_intel_sparkColumnarPlugin_vectorized_ExpressionEvaluator_nativeEvaluate(
    JNIEnv* env, jobject obj, jlong handler_ptr, jint num_rows, jlongArray buf_addrs,
    jlongArray buf_sizes) {
  arrow::Status status;
  CodeGenerator* handler = (CodeGenerator*)handler_ptr;
  std::shared_ptr<arrow::Schema> schema;
  status = handler->getSchema(&schema);

  int in_bufs_len = env->GetArrayLength(buf_addrs);
  if (in_bufs_len != env->GetArrayLength(buf_sizes)) {
    std::string error_message =
        "nativeEvaluate: mismatch in arraylen of buf_addrs and buf_sizes";
    env->ThrowNew(io_exception_class, error_message.c_str());
  }

  jlong* in_buf_addrs = env->GetLongArrayElements(buf_addrs, 0);
  jlong* in_buf_sizes = env->GetLongArrayElements(buf_sizes, 0);

  std::shared_ptr<arrow::RecordBatch> in;
  status =
      MakeRecordBatch(schema, num_rows, in_buf_addrs, in_buf_sizes, in_bufs_len, &in);

  std::shared_ptr<arrow::RecordBatch> out;
  status = handler->evaluate(in, &out);

  if (!status.ok()) {
    std::string error_message =
        "nativeEvaluate: evaluate failed with error msg " + status.ToString();
    env->ThrowNew(io_exception_class, error_message.c_str());
    return nullptr;
  }

  jobjectArray field_array =
      env->NewObjectArray(schema->num_fields(), arrow_field_node_builder_class, nullptr);

  std::vector<std::shared_ptr<arrow::Buffer>> buffers;
  for (int i = 0; i < schema->num_fields(); ++i) {
    auto column = out->column(i);
    auto dataArray = column->data();
    jobject field = env->NewObject(arrow_field_node_builder_class,
                                   arrow_field_node_builder_constructor, column->length(),
                                   column->null_count());
    env->SetObjectArrayElement(field_array, i, field);

    for (auto& buffer : dataArray->buffers) {
      buffers.push_back(buffer);
    }
  }

  jobjectArray arrowbuf_builder_array =
      env->NewObjectArray(buffers.size(), arrowbuf_builder_class, nullptr);

  for (size_t j = 0; j < buffers.size(); ++j) {
    auto buffer = buffers[j];
    uint8_t* data = nullptr;
    int size = 0;
    int64_t capacity = 0;
    if (buffer != nullptr) {
      data = (uint8_t*)buffer->data();
      size = (int)buffer->size();
      capacity = buffer->capacity();
    }
    jobject arrowBufBuilder =
        env->NewObject(arrowbuf_builder_class, arrowbuf_builder_constructor,
                       buffer_holder_.Insert(buffer), data, size, capacity);
    env->SetObjectArrayElement(arrowbuf_builder_array, j, arrowBufBuilder);
  }

  // create RecordBatch
  jobject arrowRecordBatchBuilder = env->NewObject(
      arrow_record_batch_builder_class, arrow_record_batch_builder_constructor,
      out->num_rows(), field_array, arrowbuf_builder_array);

  env->ReleaseLongArrayElements(buf_addrs, in_buf_addrs, JNI_ABORT);
  env->ReleaseLongArrayElements(buf_sizes, in_buf_sizes, JNI_ABORT);

  return arrowRecordBatchBuilder;
}

JNIEXPORT void JNICALL
Java_com_intel_sparkColumnarPlugin_vectorized_AdaptorReferenceManager_nativeRelease(
    JNIEnv* env, jobject this_obj, jlong id) {
  buffer_holder_.Erase(id);
}

#ifdef __cplusplus
}
#endif
