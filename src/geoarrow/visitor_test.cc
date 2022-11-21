#include <stdexcept>

#include <gtest/gtest.h>

#include "geoarrow.h"

TEST(VisitorTest, VisitorTestVoid) {
  struct GeoArrowVisitor v;
  GeoArrowVisitorInitVoid(&v);
  EXPECT_EQ(v.private_data, nullptr);
  EXPECT_EQ(v.reserve_feat(&v, 0), GEOARROW_OK);
  EXPECT_EQ(v.reserve_coord(&v, 0), GEOARROW_OK);
  EXPECT_EQ(v.feat_start(&v), GEOARROW_OK);
  EXPECT_EQ(v.geom_start(&v, GEOARROW_GEOMETRY_TYPE_POINT, GEOARROW_DIMENSIONS_XY),
            GEOARROW_OK);
  EXPECT_EQ(v.ring_start(&v), GEOARROW_OK);
  EXPECT_EQ(v.coords(&v, nullptr, 0, 2), GEOARROW_OK);
  EXPECT_EQ(v.ring_end(&v), GEOARROW_OK);
  EXPECT_EQ(v.geom_end(&v), GEOARROW_OK);
  EXPECT_EQ(v.feat_end(&v), GEOARROW_OK);
}
