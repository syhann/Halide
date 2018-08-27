#include "Halide.h"

using namespace Halide;

class DmaPipeline : public Generator<DmaPipeline> {
public:
    Input<Buffer<uint16_t>> input_y{"input_y", 2};
    Input<Buffer<uint16_t>> input_uv{"input_uv", 3};
    Output<Buffer<uint16_t>> output_y{"output_y", 2};
    Output<Buffer<uint16_t>> output_uv{"output_uv", 3};

    void generate() {
        Var x{"x"}, y{"y"}, c{"c"};

        // We need a wrapper for the output so we can schedule the
        // multiply update in tiles.
        Func input_copy_y("input_copy_y"), output_copy_y("output_copy_y");
        Func work_y("work_y");

        Func input_copy_uv("input_copy_uv"), output_copy_uv("output_copy_uv");
        Func work_uv("work_uv");

        input_copy_y(x, y) = input_y(x, y);
        work_y(x, y) = input_copy_y(x, y)  * 2;
        output_copy_y(x, y) = work_y(x, y);
        output_y(x, y) = output_copy_y(x, y);

        input_copy_uv(x, y, c) = input_uv(x, y, c);
        work_uv(x, y, c) = input_copy_uv(x, y, c)  * 2;
        output_copy_uv(x, y, c) = work_uv(x, y, c);
        output_uv(x, y, c) = output_copy_uv(x, y, c);

        Var tx("tx"), ty("ty");

        // Break the output into tiles.
        const int tile_width = 64;
        const int tile_height = 32;

          // tweak stride/extent to handle UV deinterleaving
        input_uv.dim(0).set_stride(2);
        input_uv.dim(2).set_stride(1).set_bounds(0, 2);
        output_uv.dim(0).set_stride(2);
        output_uv.dim(2).set_stride(1).set_bounds(0, 2);

        output_y.compute_root()
                .tile(x, y, tx, ty, x, y, tile_width, tile_height, TailStrategy::RoundUp);

        output_uv.compute_root()
                 .reorder(c, x, y)   // to handle UV interleave, with 'c' inner most loop, as DMA'd into buffer
                 .bound(c, 0, 2)
                 .tile(x, y, tx, ty, x, y, tile_width, tile_height, TailStrategy::RoundUp);

        // Schedule the copy to be computed at tiles with a
        // circular buffer of two tiles.
        input_copy_y.compute_at(output_y, tx)
                    .store_at(output_y, ty) 
                    .copy_to_host().fold_storage(x, tile_width * 2);

        Stage(output_y)
            .set_dim_device_api(tx, DeviceAPI::HexagonDma);


        work_y.compute_at(output_y, tx);

        output_copy_y
            .compute_at(output_y, tx)
            .copy_to_device();

        input_copy_uv.compute_at(output_uv, tx)
                     .store_at(output_uv, ty)
                     .bound(c, 0, 2)
                     .copy_to_host()
                     .reorder_storage(c, x, y)
                     .fold_storage(x, tile_width * 2);

        Stage(output_uv)
            .set_dim_device_api(tx, DeviceAPI::HexagonDma);

        work_uv.compute_at(output_uv, tx)
               .bound(c, 0, 2)
               .reorder_storage(c, x, y);

        output_copy_uv
            .compute_at(output_uv, tx)
            .bound(c, 0, 2)
            .copy_to_device()
            .reorder_storage(c, x, y);
    }

};

HALIDE_REGISTER_GENERATOR(DmaPipeline, pipeline_p010_linear_rw_fold)
