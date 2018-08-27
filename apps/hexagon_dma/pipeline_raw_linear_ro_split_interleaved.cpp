#include "Halide.h"

using namespace Halide;

class DmaPipeline : public Generator<DmaPipeline> {
public:
    Input<Buffer<uint8_t>> input{"input", 3};
    Output<Buffer<uint8_t>> output{"output", 3};

    void generate() {
        Var x{"x"}, y{"y"}, c{"c"};

        // We need a wrapper for the output so we can schedule the
        // multiply update in tiles.
        Func copy("copy");

        copy(x, y, c) = input(x, y, c);

        output(x, y, c) = copy(x, y, c) * 2;

        input.dim(0).set_stride(4);
        output.dim(0).set_stride(4);  

        Var tx("tx"), ty("ty");
        Var ta("ta"), tb("tb");

        // Break the output into tiles.
        const int tile_width = 256;
        const int tile_height = 128;

        Expr fac = output.dim(1).extent()/2;
        Var yo, yi;
        output.split(y, yo, yi, fac);
        
        output
            .compute_root()
            .reorder(c, x, yi)
            .bound(c, 0, 4)
            .tile(x, yi, tx, ty, ta, tb, tile_width, tile_height, TailStrategy::RoundUp)
            .parallel(yo);

        // Schedule the copy to be computed at tiles with a
        // circular buffer of two tiles.
        copy
            .compute_at(output, tx)
            .store_at(output, tx)
            .bound(c, 0, 4)
            .copy_to_host()
            .reorder_storage(c, x, y);
    }

};

HALIDE_REGISTER_GENERATOR(DmaPipeline, pipeline_raw_linear_ro_split_interleaved)
