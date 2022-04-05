LZ4RAW - headerless LZ4 compressor
====================================

LZ4RAW is a modification of the LZ4 program to remove all header generation. The reason for this port is to support old or minimal hardware, where header parsing isn't desired. Your target decompressor must also skip headers, and be explicity provided the target size, since this is normally pulled from the headers.

Files generated by LZ4RAW aren't compliant with any other LZ4 standard or utility. If you run into problems, please don't bother the official LZ4 folks.
