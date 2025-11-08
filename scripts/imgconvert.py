#!python3

from PIL import Image, ImageOps
from argparse import ArgumentParser
import sys
import math

SCREEN_WIDTH = 1200
SCREEN_HEIGHT = 825

if SCREEN_WIDTH % 2:
    print("image width must be even!", file=sys.stderr)
    sys.exit(1)

parser = ArgumentParser()
parser.add_argument('-i', action="store", dest="inputfile")
parser.add_argument('-n', action="store", dest="name")
parser.add_argument('-o', action="store", dest="outputfile")
parser.add_argument('--invert', action="store_true", dest="invert", help="Invert image colors")

args = parser.parse_args()

im = Image.open(args.inputfile)
# convert to grayscale
im = im.convert(mode='L')

# Invert colors if requested
if args.invert:
    im = ImageOps.invert(im)

# Use LANCZOS instead of deprecated ANTIALIAS
try:
    im.thumbnail((SCREEN_WIDTH, SCREEN_HEIGHT), Image.Resampling.LANCZOS)
except AttributeError:
    # Fallback for older Pillow versions
    im.thumbnail((SCREEN_WIDTH, SCREEN_HEIGHT), Image.LANCZOS)

# Write out the output file.
with open(args.outputfile, 'w') as f:
    # Generate include guard name from filename
    guard_name = args.name.upper() + "_H"
    
    # Write header with include guard
    f.write("#ifndef {}\n".format(guard_name))
    f.write("#define {}\n\n".format(guard_name))
    f.write("#include <epd_driver.h>\n\n")
    
    f.write("static const uint8_t {}_data[({}*{})/2] = {{\n".format(args.name, math.ceil(im.size[0] / 2) * 2, im.size[1]))
    for y in range(0, im.size[1]):
        byte = 0
        done = True
        for x in range(0, im.size[0]):
            l = im.getpixel((x, y))
            if x % 2 == 0:
                byte = l >> 4
                done = False;
            else:
                byte |= l & 0xF0
                f.write("0x{:02X}, ".format(byte))
                done = True
        if not done:
            f.write("0x{:02X}, ".format(byte))
        f.write("\n\t");
    f.write("};\n\n")
    
    # Write the GFXimage struct
    f.write("static const GFXimage {} = {{\n".format(args.name))
    f.write("    .width = {},\n".format(im.size[0]))
    f.write("    .height = {},\n".format(im.size[1]))
    f.write("    .data = (uint8_t*){}_data\n".format(args.name))
    f.write("};\n\n")
    
    f.write("#endif // {}\n".format(guard_name))
