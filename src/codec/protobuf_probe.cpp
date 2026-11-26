#include "protobuf_probe.h"

#include <pb_decode.h>
#include <pb_encode.h>
#include "fortisnet/intelligence/v1/dashpay.pb.h"

namespace fortis::codec {

bool protobuf_roundtrip(const uint8_t* input, size_t input_size,
                        uint8_t* output, size_t output_capacity,
                        size_t* output_size) {
  if (!input || !output || !output_size) return false;
  fortisnet_intelligence_v1_DashPayNameResponse message =
      fortisnet_intelligence_v1_DashPayNameResponse_init_zero;
  pb_istream_t input_stream = pb_istream_from_buffer(input, input_size);
  if (!pb_decode(&input_stream, fortisnet_intelligence_v1_DashPayNameResponse_fields,
                 &message)) return false;
  pb_ostream_t output_stream = pb_ostream_from_buffer(output, output_capacity);
  if (!pb_encode(&output_stream, fortisnet_intelligence_v1_DashPayNameResponse_fields,
                 &message)) return false;
  *output_size = output_stream.bytes_written;
  return true;
}

}  // namespace fortis::codec
