
#define R_NO_REMAP
#include <R.h>
#include <Rinternals.h>

#include <vector>
#include <stdexcept>

#include "geoarrow.h"
#include "nanoarrow.h"
#include "wk-v1.h"

// Helper to translate between the GeoArrowVisitor and the wk_handler_t.
class WKGeoArrowHandler {
 public:
  WKGeoArrowHandler(wk_handler_t* handler, R_xlen_t size)
      : handler_(handler), feat_id_(-1), ring_id_(-1), coord_id_(-1) {
    WK_VECTOR_META_RESET(vector_meta_, WK_GEOMETRY);
    WK_META_RESET(meta_, WK_GEOMETRY);

    vector_meta_.size = size;

    // This is to keep vectors from being reallocated, since some
    // wk handlers assume that the meta pointers will stay valid between
    // the start and end geometry methods (this will get fixed in a
    // wk release soon)
    part_id_stack_.reserve(32);
    meta_stack_.reserve(32);
  }

  // Visitor interface
  void InitVisitor(struct GeoArrowVisitor* v) {
    v->feat_start = &feat_start_visitor;
    v->null_feat = &null_feat_visitor;
    v->geom_start = &geom_start_visitor;
    v->ring_start = &ring_start_visitor;
    v->coords = &coords_visitor;
    v->ring_end = &ring_end_visitor;
    v->geom_end = &geom_end_visitor;
    v->feat_end = &feat_end_visitor;
    v->private_data = this;
  }

  void set_vector_geometry_type(GeoArrowGeometryType geometry_type) {
    vector_meta_.geometry_type = geometry_type;
  }

  void set_vector_dimensions(GeoArrowDimensions dimensions) {
    vector_meta_.flags &= ~WK_FLAG_HAS_Z;
    vector_meta_.flags &= ~WK_FLAG_HAS_M;

    switch (dimensions) {
      case GEOARROW_DIMENSIONS_XYZ:
      case GEOARROW_DIMENSIONS_XYZM:
        vector_meta_.flags |= WK_FLAG_HAS_Z;
        break;
      default:
        break;
    }

    switch (dimensions) {
      case GEOARROW_DIMENSIONS_XYM:
      case GEOARROW_DIMENSIONS_XYZM:
        vector_meta_.flags |= WK_FLAG_HAS_M;
        break;
      default:
        break;
    }

    if (dimensions == GEOARROW_DIMENSIONS_UNKNOWN) {
      vector_meta_.flags |= WK_FLAG_DIMS_UNKNOWN;
    } else {
      vector_meta_.flags &= ~WK_FLAG_DIMS_UNKNOWN;
    }
  }

  void set_meta_dimensions(GeoArrowDimensions dimensions) {
    meta_.flags &= ~WK_FLAG_HAS_Z;
    meta_.flags &= ~WK_FLAG_HAS_M;

    switch (dimensions) {
      case GEOARROW_DIMENSIONS_XYZ:
      case GEOARROW_DIMENSIONS_XYZM:
        meta_.flags |= WK_FLAG_HAS_Z;
        break;
      default:
        break;
    }

    switch (dimensions) {
      case GEOARROW_DIMENSIONS_XYM:
      case GEOARROW_DIMENSIONS_XYZM:
        meta_.flags |= WK_FLAG_HAS_M;
        break;
      default:
        break;
    }
  }

  int feat_start() {
    feat_id_++;
    part_id_stack_.clear();
    meta_stack_.clear();
    return handler_->feature_start(&vector_meta_, feat_id_, handler_->handler_data);
  }

  int null_feat() { return handler_->null_feature(handler_->handler_data); }

  int geom_start(GeoArrowGeometryType geometry_type, GeoArrowDimensions dimensions,
                 uint32_t size) {
    ring_id_ = -1;
    coord_id_ = -1;

    if (part_id_stack_.size() > 0) {
      part_id_stack_[part_id_stack_.size() - 1]++;
    }

    meta_.geometry_type = geometry_type;
    meta_.size = size;
    set_meta_dimensions(dimensions);
    meta_stack_.push_back(meta_);

    int result = handler_->geometry_start(meta(), part_id(), handler_->handler_data);
    part_id_stack_.push_back(-1);
    return result;
  }

  int ring_start(uint32_t size) {
    ring_id_++;
    coord_id_ = -1;
    ring_size_ = size;
    return handler_->ring_start(meta(), ring_size_, ring_id_, handler_->handler_data);
  }

  int coords(const struct GeoArrowCoordView* coords) {
    int result;
    double coord[4];
    for (int64_t i = 0; i < coords->n_coords; i++) {
      coord_id_++;
      for (int j = 0; j < coords->n_values; j++) {
        coord[j] = GEOARROW_COORD_VIEW_VALUE(coords, i, j);
      }

      result = handler_->coord(meta(), coord, coord_id_, handler_->handler_data);
      if (result != WK_CONTINUE) {
        return result;
      }
    }

    return WK_CONTINUE;
  }

  int ring_end() {
    return handler_->ring_end(meta(), ring_size_, ring_id_, handler_->handler_data);
  }

  int geom_end() {
    if (part_id_stack_.size() > 0) part_id_stack_.pop_back();
    int result = handler_->geometry_end(meta(), part_id(), handler_->handler_data);
    if (meta_stack_.size() > 0) meta_stack_.pop_back();
    return (int)result;
  }

  int feat_end() {
    return handler_->feature_end(&vector_meta_, feat_id_, handler_->handler_data);
  }

  wk_vector_meta_t vector_meta_;

 private:
  wk_handler_t* handler_;

  std::vector<wk_meta_t> meta_stack_;
  std::vector<int32_t> part_id_stack_;
  wk_meta_t meta_;

  int32_t ring_size_;
  int64_t feat_id_;

  int32_t ring_id_;
  int32_t coord_id_;

  int32_t part_id() {
    if (part_id_stack_.size() == 0) {
      return WK_PART_ID_NONE;
    } else {
      return part_id_stack_[part_id_stack_.size() - 1];
    }
  }

  const wk_meta_t* meta() {
    if (meta_stack_.size() == 0) {
      throw std::runtime_error("geom_start()/geom_end() stack imbalance <meta>");
    }
    return meta_stack_.data() + meta_stack_.size() - 1;
  }

  static int wrap_result(int result, GeoArrowError* error) {
    if (result == WK_ABORT_FEATURE) {
      GeoArrowErrorSet(error, "WK_ABORT_FEATURE not supported");
      return EINVAL;
    }

    if (result != WK_CONTINUE) {
      GeoArrowErrorSet(error, "result !+ WK_CONTINUE (%d)", result);
      return EINVAL;
    } else {
      return GEOARROW_OK;
    }
  }

  static int feat_start_visitor(struct GeoArrowVisitor* v) {
    auto private_data = reinterpret_cast<WKGeoArrowHandler*>(v->private_data);
    int result = private_data->feat_start();
    return wrap_result(result, v->error);
  }

  static int null_feat_visitor(struct GeoArrowVisitor* v) {
    auto private_data = reinterpret_cast<WKGeoArrowHandler*>(v->private_data);
    int result = private_data->null_feat();
    return wrap_result(result, v->error);
  }

  static int geom_start_visitor(struct GeoArrowVisitor* v,
                                enum GeoArrowGeometryType geometry_type,
                                enum GeoArrowDimensions dimensions) {
    auto private_data = reinterpret_cast<WKGeoArrowHandler*>(v->private_data);
    int result = private_data->geom_start(geometry_type, dimensions, WK_SIZE_UNKNOWN);
    return wrap_result(result, v->error);
  }

  static int ring_start_visitor(struct GeoArrowVisitor* v) {
    auto private_data = reinterpret_cast<WKGeoArrowHandler*>(v->private_data);
    int result = private_data->ring_start(WK_SIZE_UNKNOWN);
    return wrap_result(result, v->error);
  }

  static int coords_visitor(struct GeoArrowVisitor* v,
                            const struct GeoArrowCoordView* coords) {
    auto private_data = reinterpret_cast<WKGeoArrowHandler*>(v->private_data);
    int result = private_data->coords(coords);
    return wrap_result(result, v->error);
  }

  static int ring_end_visitor(struct GeoArrowVisitor* v) {
    auto private_data = reinterpret_cast<WKGeoArrowHandler*>(v->private_data);
    int result = private_data->ring_end();
    return wrap_result(result, v->error);
  }

  static int geom_end_visitor(struct GeoArrowVisitor* v) {
    auto private_data = reinterpret_cast<WKGeoArrowHandler*>(v->private_data);
    int result = private_data->geom_end();
    return wrap_result(result, v->error);
  }

  static int feat_end_visitor(struct GeoArrowVisitor* v) {
    auto private_data = reinterpret_cast<WKGeoArrowHandler*>(v->private_data);
    int result = private_data->feat_end();
    return wrap_result(result, v->error);
  }
};

static void finalize_wk_geoarrow_handler_xptr(SEXP xptr) {
  auto private_data = reinterpret_cast<WKGeoArrowHandler*>(R_ExternalPtrAddr(xptr));
  delete private_data;
}

static void finalize_array_reader_xptr(SEXP xptr) {
  auto ptr = reinterpret_cast<GeoArrowArrayReader*>(R_ExternalPtrAddr(xptr));
  if (ptr != NULL) {
    GeoArrowArrayReaderReset(ptr);
  }

  ArrowFree(ptr);
}

SEXP geoarrow_handle_stream(SEXP data, wk_handler_t* handler) {
  auto array_stream =
      reinterpret_cast<struct ArrowArrayStream*>(R_ExternalPtrAddr(VECTOR_ELT(data, 0)));
  auto schema =
      reinterpret_cast<struct ArrowSchema*>(R_ExternalPtrAddr(VECTOR_ELT(data, 1)));
  auto array =
      reinterpret_cast<struct ArrowArray*>(R_ExternalPtrAddr(VECTOR_ELT(data, 2)));
  SEXP n_features_sexp = VECTOR_ELT(data, 3);

  R_xlen_t vector_size = WK_VECTOR_SIZE_UNKNOWN;
  if (TYPEOF(n_features_sexp) == INTSXP) {
    if (INTEGER(n_features_sexp)[0] != NA_INTEGER) {
      vector_size = INTEGER(n_features_sexp)[0];
    }
  } else {
    double n_features_double = REAL(n_features_sexp)[0];
    if (!ISNA(n_features_double) && !ISNAN(n_features_double)) {
      vector_size = n_features_double;
    }
  }

  // Initialize the schema_view
  struct GeoArrowSchemaView schema_view;
  struct GeoArrowError error;
  int errno_code = GeoArrowSchemaViewInit(&schema_view, schema, &error);
  if (errno_code != GEOARROW_OK) {
    Rf_error("[GeoArrowSchemaViewInit] %s", error.message);
  }

  // Initialize the array view
  struct GeoArrowArrayView array_view;
  errno_code = GeoArrowArrayViewInitFromSchema(&array_view, schema, &error);
  if (errno_code != GEOARROW_OK) {
    Rf_error("[GeoArrowArrayViewInitFromSchema] %s", error.message);
  }

  // Initialize the reader + make sure it is always cleaned up
  struct GeoArrowArrayReader* reader = reinterpret_cast<struct GeoArrowArrayReader*>(
      ArrowMalloc(sizeof(GeoArrowArrayReader)));
  if (reader == NULL) {
    Rf_error("Failed to malloc sizeof(GeoArrowArrayReader)");
  }
  memset(reader, 0, sizeof(struct GeoArrowArrayReader));
  SEXP reader_xptr = PROTECT(R_MakeExternalPtr(reader, R_NilValue, R_NilValue));
  R_RegisterCFinalizer(reader_xptr, &finalize_array_reader_xptr);

  errno_code = GeoArrowArrayReaderInit(reader);
  if (errno_code != GEOARROW_OK) {
    Rf_error("GeoArrowArrayReaderInit() failed");
  }

  // Instantiate + protect the adapter
  auto adapter = new WKGeoArrowHandler(handler, vector_size);
  SEXP adapter_xptr = PROTECT(R_MakeExternalPtr(adapter, R_NilValue, R_NilValue));
  R_RegisterCFinalizer(adapter_xptr, &finalize_wk_geoarrow_handler_xptr);
  adapter->set_vector_dimensions(schema_view.dimensions);
  adapter->set_vector_geometry_type(schema_view.geometry_type);

  // Initialize the visitor
  struct GeoArrowVisitor visitor;
  adapter->InitVisitor(&visitor);
  visitor.error = &error;

  int result = handler->vector_start(&adapter->vector_meta_, handler->handler_data);
  if (result == WK_CONTINUE) {
    while (true) {
      if (array->release != NULL) {
        array->release(array);
      }

      // Get the next array
      errno_code = array_stream->get_next(array_stream, array);
      if (errno_code != 0) {
        const char* error_message = array_stream->get_last_error(array_stream);
        if (error_message != NULL) {
          Rf_error("[array_stream->get_next] %s", errno_code, error_message);
        } else {
          Rf_error("[array_stream->get_next] failed with code %d", errno_code);
        }
      }

      // End of stream
      if (array->release == NULL) {
        break;
      }

      // We have a valid array: set the array in the visitor
      errno_code = GeoArrowArrayViewSetArray(&array_view, array, &error);
      if (errno_code != GEOARROW_OK) {
        Rf_error("[GeoArrowArrayViewSetArray] %s", error.message);
      }

      // ...and visit!
      errno_code =
          GeoArrowArrayReaderVisit(reader, &array_view, 0, array->length, &visitor);
      if (errno_code != GEOARROW_OK) {
        Rf_error("[GeoArrowArrayViewVisit] %s", error.message);
      }

      // Check for cancel
      R_CheckUserInterrupt();
    }
  }

  SEXP result_sexp =
      PROTECT(handler->vector_end(&adapter->vector_meta_, handler->handler_data));
  UNPROTECT(3);
  return result_sexp;
}

extern "C" SEXP geoarrow_c_handle_stream(SEXP data, SEXP handler_xptr) {
  return wk_handler_run_xptr(&geoarrow_handle_stream, data, handler_xptr);
}
