// A reader for .7z archives, written for the setup's one job: unpacking RPCS3's Windows build (_setup\rpcs3) on a Windows
// whose own tar cannot read the 7z format (Windows 10's tar has no 7z reader; Windows 11's has one). Nothing outside
// this file is needed: no 7-Zip, no download, no library.
//   Methods:   Copy, LZMA, LZMA2, BCJ (x86), BCJ2 - the ones 7-Zip chooses for program files; solid blocks, several
//              blocks, encoded headers. Every file is checked against the CRC32 the archive holds for it.
//   Not here:  encryption and the other filters (an archive that needs them stops with a clear error).
// The decoders follow the LZMA SDK by Igor Pavlov, which is public domain, and are written again in C# 5 (the compiler
// that is part of Windows; see build_launcher.bat): the range decoder and the LZMA model of LzmaDec.c, the chunk
// format of Lzma2Dec.c, the x86 converter of Bra86.c, the BCJ2 decoder of Bcj2.c. [GT6 0090]
using System;
using System.Collections.Generic;
using System.IO;
using System.Runtime.CompilerServices;
using System.Text;

namespace GT6Launcher
{
    public static class SevenZip
    {
        // Unpacks every file of the archive below targetDir (made if needed) and returns how many files it made.
        // progress(bytesDone, bytesTotal) and cancelled() may be null; a cancelled() that says true ends the
        // unpacking with an OperationCanceledException. A damaged archive throws InvalidDataException, a method this
        // reader does not have throws NotSupportedException.
        public static int Extract(string archive, string targetDir, Action<long, long> progress, Func<bool> cancelled)
        {
            return new SzArchive(archive).ExtractAll(targetDir, progress, cancelled);
        }
    }

    // ---------------------------------------------------------------------------------------------------------------
    // The container
    // ---------------------------------------------------------------------------------------------------------------

    sealed class SzCoder
    {
        public ulong Method;
        public int NumIn, NumOut;        // in the archive's words: In = the packed side of the decoder, Out = the unpacked side
        public byte[] Props;
    }

    sealed class SzFolder             // a "block": coders wired together that turn pack streams into one unpacked stream
    {
        public SzCoder[] Coders;
        public int[] BindIn, BindOut;    // coder input BindIn[k] is fed by coder output BindOut[k]
        public int[] PackedIn;           // the coder inputs that are fed by pack streams, in the order of those streams
        public int[] FirstIn, FirstOut;  // per coder: the index of its first input / output
        public int TotalOut;
        public long[] OutSizes;          // per coder output
        public bool HasCrc;
        public uint Crc;
        public int FirstPackStream;      // index of the folder's first pack stream in the archive

        public int MainOut               // the one output no other coder takes: the folder's result
        {
            get
            {
                for (int o = 0; o < TotalOut; o++)
                    if (Array.IndexOf(BindOut, o) < 0)
                        return o;
                throw new InvalidDataException("7z: a folder without a result");
            }
        }

        public long UnpackSize
        {
            get { return OutSizes[MainOut]; }
        }
    }

    sealed class SzStreams
    {
        public long PackPos;
        public long[] PackSizes = new long[0];
        public long[] PackOffsets = new long[0];             // offset of each pack stream in the file
        public SzFolder[] Folders = new SzFolder[0];
        public int[] NumSub = new int[0];                    // files in each folder
        public long[] SubSizes = new long[0];
        public bool[] SubHasCrc = new bool[0];
        public uint[] SubCrc = new uint[0];
    }

    sealed class SzFile
    {
        public string Name;
        public bool HasStream, IsDir, IsAnti, HasCrc;
        public long Size, MTime;         // MTime: a FILETIME, 0 = none
        public uint Crc;
        public int Folder;
    }

    sealed class SzHeaderReader
    {
        readonly byte[] b;
        public int Pos;

        public SzHeaderReader(byte[] data)
        {
            b = data;
        }

        public int Length { get { return b.Length; } }

        static Exception Bad()
        {
            return new InvalidDataException("7z: the header is damaged");
        }

        public byte Byte()
        {
            if (Pos >= b.Length)
                throw Bad();
            return b[Pos++];
        }

        public ulong Number()            // 7z's variable length number: the leading 1 bits of the first byte say how many bytes follow
        {
            byte first = Byte();
            byte mask = 0x80;
            ulong value = 0;
            for (int i = 0; i < 8; i++)
            {
                if ((first & mask) == 0)
                    return value + ((ulong)(first & (mask - 1)) << (8 * i));
                value |= (ulong)Byte() << (8 * i);
                mask >>= 1;
            }
            return value;
        }

        public long Long()
        {
            ulong v = Number();
            if (v > long.MaxValue)
                throw Bad();
            return (long)v;
        }

        public int Int()
        {
            ulong v = Number();
            if (v > 0x7FFFFFFF)
                throw Bad();
            return (int)v;
        }

        public uint UInt32()
        {
            uint v = 0;
            for (int i = 0; i < 4; i++)
                v |= (uint)Byte() << (8 * i);
            return v;
        }

        public ulong UInt64()
        {
            ulong v = 0;
            for (int i = 0; i < 8; i++)
                v |= (ulong)Byte() << (8 * i);
            return v;
        }

        public byte[] Bytes(int count)
        {
            if (count < 0 || count > b.Length - Pos)
                throw Bad();
            var r = new byte[count];
            Buffer.BlockCopy(b, Pos, r, 0, count);
            Pos += count;
            return r;
        }

        public void SkipRecord()         // an unknown record: its size, then its bytes
        {
            long size = Long();
            if (size > b.Length - Pos)
                throw Bad();
            Pos += (int)size;
        }

        public bool[] BitVector(int count)
        {
            var v = new bool[count];
            int mask = 0, cur = 0;
            for (int i = 0; i < count; i++)
            {
                if (mask == 0)
                {
                    cur = Byte();
                    mask = 0x80;
                }
                v[i] = (cur & mask) != 0;
                mask >>= 1;
            }
            return v;
        }

        public bool[] OptionalBitVector(int count)   // one byte "all defined", else the vector
        {
            if (Byte() != 0)
            {
                var all = new bool[count];
                for (int i = 0; i < count; i++)
                    all[i] = true;
                return all;
            }
            return BitVector(count);
        }

        public void Digests(int count, out bool[] defined, out uint[] crc)
        {
            defined = OptionalBitVector(count);
            crc = new uint[count];
            for (int i = 0; i < count; i++)
                if (defined[i])
                    crc[i] = UInt32();
        }

        public string WideString()       // UTF-16, ended by a zero character
        {
            int start = Pos;
            while (true)
            {
                if (Pos + 1 >= b.Length)
                    throw Bad();
                if (b[Pos] == 0 && b[Pos + 1] == 0)
                    break;
                Pos += 2;
            }
            string s = Encoding.Unicode.GetString(b, start, Pos - start);
            Pos += 2;
            return s;
        }
    }

    sealed class SzArchive
    {
        const int StartHeaderSize = 32;
        readonly string path;
        SzStreams main = new SzStreams();
        SzFile[] files = new SzFile[0];
        long fileLength;

        // for ExtractAll
        Action<long, long> progress;
        Func<bool> cancelled;
        long done, total;

        static Exception Bad(string what)
        {
            return new InvalidDataException("7z: " + what);
        }

        public SzArchive(string path)
        {
            this.path = path;
            ReadHeaders();
        }

        void ReadHeaders()
        {
            byte[] start = new byte[StartHeaderSize];
            byte[] header;
            using (var f = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite))
            {
                fileLength = f.Length;
                if (SzStream.ReadSome(f, start, 0, StartHeaderSize) != StartHeaderSize)
                    throw Bad("the file is too short");
                byte[] signature = { 0x37, 0x7A, 0xBC, 0xAF, 0x27, 0x1C };
                for (int i = 0; i < signature.Length; i++)
                    if (start[i] != signature[i])
                        throw Bad("not a 7z archive");
                if (start[6] != 0)
                    throw new NotSupportedException("7z: format version " + start[6]);
                if (SzCrc32.Of(start, 12, 20) != Le32(start, 8))
                    throw Bad("the start header is damaged");
                long nextOffset = (long)Le64(start, 12), nextSize = (long)Le64(start, 20);
                uint nextCrc = Le32(start, 28);
                if (nextSize == 0)
                    return;                                   // an empty archive
                if (nextOffset < 0 || nextSize < 0 || nextSize > (1L << 28) || StartHeaderSize + nextOffset + nextSize > fileLength)
                    throw Bad("the archive ends early (the header is beyond the end of the file)");
                header = new byte[nextSize];
                f.Position = StartHeaderSize + nextOffset;
                if (SzStream.ReadSome(f, header, 0, header.Length) != header.Length)
                    throw Bad("the archive ends early");
                if (SzCrc32.Of(header, 0, header.Length) != nextCrc)
                    throw Bad("the header's CRC does not match");
            }
            for (int round = 0; round < 4; round++)
            {
                var r = new SzHeaderReader(header);
                byte id = r.Byte();
                if (id == 0x01)
                {
                    ReadMainHeader(r);
                    return;
                }
                if (id != 0x17)
                    throw Bad("unknown header type");
                // an encoded header: it is itself packed, as a block of the archive
                SzStreams si = ReadStreamsInfo(r);
                if (si.Folders.Length < 1)
                    throw Bad("an encoded header without a block");
                header = ReadWhole(si, si.Folders[0]);
            }
            throw Bad("the header is nested too deep");
        }

        byte[] ReadWhole(SzStreams si, SzFolder folder)
        {
            long size = folder.UnpackSize;
            if (size < 0 || size > (1L << 28))
                throw Bad("the encoded header is too big");
            var owned = new List<IDisposable>();
            try
            {
                Stream s = OpenOut(si, folder, folder.MainOut, owned, 0);
                var data = new byte[size];
                if (SzStream.ReadSome(s, data, 0, data.Length) != data.Length)
                    throw Bad("the encoded header ends early");
                if (folder.HasCrc && SzCrc32.Of(data, 0, data.Length) != folder.Crc)
                    throw Bad("the encoded header's CRC does not match");
                return data;
            }
            finally
            {
                foreach (IDisposable d in owned)
                    d.Dispose();
            }
        }

        static uint Le32(byte[] b, int at)
        {
            return (uint)(b[at] | (b[at + 1] << 8) | (b[at + 2] << 16) | (b[at + 3] << 24));
        }

        static ulong Le64(byte[] b, int at)
        {
            return Le32(b, at) | ((ulong)Le32(b, at + 4) << 32);
        }

        void ReadMainHeader(SzHeaderReader r)
        {
            while (true)
            {
                byte id = r.Byte();
                if (id == 0x00)
                    return;
                if (id == 0x02)                  // archive properties: skipped
                {
                    while (r.Number() != 0)
                        r.SkipRecord();
                }
                else if (id == 0x03)             // additional streams: not used by anything that writes archives today
                    ReadStreamsInfo(r);
                else if (id == 0x04)
                    main = ReadStreamsInfo(r);
                else if (id == 0x05)
                    ReadFilesInfo(r);
                else
                    throw Bad("unknown record in the header");
            }
        }

        SzStreams ReadStreamsInfo(SzHeaderReader r)
        {
            var si = new SzStreams();
            byte id = r.Byte();
            if (id == 0x06)                      // where the packed streams are
            {
                si.PackPos = r.Long();
                int n = r.Int();
                if (n > r.Length)
                    throw Bad("too many pack streams");
                si.PackSizes = new long[n];
                while (true)
                {
                    id = r.Byte();
                    if (id == 0x00)
                        break;
                    if (id == 0x09)
                    {
                        for (int i = 0; i < n; i++)
                            si.PackSizes[i] = r.Long();
                    }
                    else if (id == 0x0A)
                    {
                        bool[] d;
                        uint[] c;
                        r.Digests(n, out d, out c);
                    }
                    else
                        r.SkipRecord();
                }
                si.PackOffsets = new long[n];
                long at = StartHeaderSize + si.PackPos;
                for (int i = 0; i < n; i++)
                {
                    si.PackOffsets[i] = at;
                    at += si.PackSizes[i];
                    if (at < 0 || at > fileLength)
                        throw Bad("a pack stream is beyond the end of the file");
                }
                id = r.Byte();
            }
            if (id == 0x07)                      // how they are unpacked
            {
                ReadUnpackInfo(r, si);
                id = r.Byte();
            }
            if (id == 0x08)                      // how the unpacked blocks divide into files
            {
                ReadSubStreamsInfo(r, si);
                id = r.Byte();
            }
            else
                OneStreamPerFolder(si);
            if (id != 0x00)
                throw Bad("unknown record in the streams info");
            return si;
        }

        static SzFolder ReadFolder(SzHeaderReader r)
        {
            int numCoders = r.Int();
            if (numCoders < 1 || numCoders > 32)
                throw Bad("the number of coders");
            var f = new SzFolder { Coders = new SzCoder[numCoders], FirstIn = new int[numCoders], FirstOut = new int[numCoders] };
            int totalIn = 0, totalOut = 0;
            for (int i = 0; i < numCoders; i++)
            {
                byte flag = r.Byte();
                if ((flag & 0x80) != 0)
                    throw new NotSupportedException("7z: alternative coders");
                int idSize = flag & 0x0F;
                if (idSize > 8)
                    throw Bad("a coder id");
                ulong method = 0;
                for (int k = 0; k < idSize; k++)
                    method = (method << 8) | r.Byte();
                var c = new SzCoder { Method = method, NumIn = 1, NumOut = 1 };
                if ((flag & 0x10) != 0)
                {
                    c.NumIn = r.Int();
                    c.NumOut = r.Int();
                    if (c.NumIn < 1 || c.NumIn > 32 || c.NumOut < 1 || c.NumOut > 32)
                        throw Bad("the streams of a coder");
                }
                c.Props = (flag & 0x20) != 0 ? r.Bytes(r.Int()) : new byte[0];
                f.Coders[i] = c;
                f.FirstIn[i] = totalIn;
                f.FirstOut[i] = totalOut;
                totalIn += c.NumIn;
                totalOut += c.NumOut;
            }
            f.TotalOut = totalOut;
            int numBind = totalOut - 1;
            f.BindIn = new int[numBind];
            f.BindOut = new int[numBind];
            for (int i = 0; i < numBind; i++)
            {
                f.BindIn[i] = r.Int();
                f.BindOut[i] = r.Int();
                if (f.BindIn[i] >= totalIn || f.BindOut[i] >= totalOut)
                    throw Bad("a bind pair");
            }
            int numPacked = totalIn - numBind;
            if (numPacked < 1)
                throw Bad("the packed streams of a block");
            f.PackedIn = new int[numPacked];
            if (numPacked == 1)
            {
                int found = -1;
                for (int i = 0; i < totalIn && found < 0; i++)
                    if (Array.IndexOf(f.BindIn, i) < 0)
                        found = i;
                if (found < 0)
                    throw Bad("a block without a packed stream");
                f.PackedIn[0] = found;
            }
            else
                for (int i = 0; i < numPacked; i++)
                {
                    f.PackedIn[i] = r.Int();
                    if (f.PackedIn[i] >= totalIn)
                        throw Bad("a packed stream index");
                }
            return f;
        }

        static void ReadUnpackInfo(SzHeaderReader r, SzStreams si)
        {
            if (r.Byte() != 0x0B)
                throw Bad("the list of blocks is missing");
            int n = r.Int();
            if (n > r.Length)
                throw Bad("too many blocks");
            if (r.Byte() != 0)
                throw new NotSupportedException("7z: external block data");
            si.Folders = new SzFolder[n];
            int pack = 0;
            for (int i = 0; i < n; i++)
            {
                SzFolder f = ReadFolder(r);
                f.FirstPackStream = pack;
                pack += f.PackedIn.Length;
                si.Folders[i] = f;
            }
            if (pack > si.PackSizes.Length)
                throw Bad("a block has no pack stream");
            if (r.Byte() != 0x0C)
                throw Bad("the unpacked sizes are missing");
            foreach (SzFolder f in si.Folders)
            {
                f.OutSizes = new long[f.TotalOut];
                for (int k = 0; k < f.OutSizes.Length; k++)
                    f.OutSizes[k] = r.Long();
            }
            while (true)
            {
                byte id = r.Byte();
                if (id == 0x00)
                    return;
                if (id == 0x0A)
                {
                    bool[] def;
                    uint[] crc;
                    r.Digests(n, out def, out crc);
                    for (int i = 0; i < n; i++)
                    {
                        si.Folders[i].HasCrc = def[i];
                        si.Folders[i].Crc = crc[i];
                    }
                }
                else
                    r.SkipRecord();
            }
        }

        static void OneStreamPerFolder(SzStreams si)
        {
            int n = si.Folders.Length;
            si.NumSub = new int[n];
            si.SubSizes = new long[n];
            si.SubHasCrc = new bool[n];
            si.SubCrc = new uint[n];
            for (int i = 0; i < n; i++)
            {
                si.NumSub[i] = 1;
                si.SubSizes[i] = si.Folders[i].UnpackSize;
                si.SubHasCrc[i] = si.Folders[i].HasCrc;
                si.SubCrc[i] = si.Folders[i].Crc;
            }
        }

        static void ReadSubStreamsInfo(SzHeaderReader r, SzStreams si)
        {
            int n = si.Folders.Length;
            si.NumSub = new int[n];
            for (int i = 0; i < n; i++)
                si.NumSub[i] = 1;
            byte id = r.Byte();
            if (id == 0x0D)
            {
                for (int i = 0; i < n; i++)
                    si.NumSub[i] = r.Int();
                id = r.Byte();
            }
            long count = 0;
            for (int i = 0; i < n; i++)
                count += si.NumSub[i];
            if (count > r.Length * 8L + n)
                throw Bad("too many files in the blocks");
            int total = (int)count;
            si.SubSizes = new long[total];
            si.SubHasCrc = new bool[total];
            si.SubCrc = new uint[total];
            int k = 0;
            for (int i = 0; i < n; i++)
            {
                int num = si.NumSub[i];
                if (num == 0)
                    continue;
                long sum = 0;
                for (int j = 1; j < num; j++)
                {
                    if (id != 0x09)
                        throw Bad("the sizes of the files in a block are missing");
                    long s = r.Long();
                    si.SubSizes[k++] = s;
                    sum += s;
                }
                long rest = si.Folders[i].UnpackSize - sum;
                if (rest < 0)
                    throw Bad("the sizes of the files do not fit their block");
                si.SubSizes[k++] = rest;
            }
            if (id == 0x09)
                id = r.Byte();
            // a block with one file has that file's CRC in the block's own record; the others are listed here
            int needed = 0;
            k = 0;
            for (int i = 0; i < n; i++)
            {
                if (si.NumSub[i] == 1 && si.Folders[i].HasCrc)
                {
                    si.SubHasCrc[k] = true;
                    si.SubCrc[k] = si.Folders[i].Crc;
                }
                else
                    needed += si.NumSub[i];
                k += si.NumSub[i];
            }
            while (id != 0x00)
            {
                if (id == 0x0A)
                {
                    bool[] def;
                    uint[] crc;
                    r.Digests(needed, out def, out crc);
                    k = 0;
                    int d = 0;
                    for (int i = 0; i < n; i++)
                    {
                        if (si.NumSub[i] == 1 && si.Folders[i].HasCrc)
                            k++;
                        else
                            for (int j = 0; j < si.NumSub[i]; j++)
                            {
                                si.SubHasCrc[k] = def[d];
                                si.SubCrc[k] = crc[d];
                                k++;
                                d++;
                            }
                    }
                }
                else
                    r.SkipRecord();
                id = r.Byte();
            }
        }

        void ReadFilesInfo(SzHeaderReader r)
        {
            int n = r.Int();
            if (n > r.Length)
                throw Bad("too many files");
            var list = new SzFile[n];
            bool[] emptyStream = new bool[n], emptyFile = null, anti = null;
            int numEmpty = 0;
            string[] names = null;
            var mtimes = new long[n];
            var attrDefined = new bool[n];
            var attrs = new uint[n];
            while (true)
            {
                ulong type = r.Number();
                if (type == 0)
                    break;
                long size = r.Long();
                if (size > r.Length - r.Pos)
                    throw Bad("a file property is beyond the header");
                int end = r.Pos + (int)size;
                if (type == 0x0E)
                {
                    emptyStream = r.BitVector(n);
                    numEmpty = 0;
                    for (int i = 0; i < n; i++)
                        if (emptyStream[i])
                            numEmpty++;
                }
                else if (type == 0x0F)
                    emptyFile = r.BitVector(numEmpty);
                else if (type == 0x10)
                    anti = r.BitVector(numEmpty);
                else if (type == 0x11)
                {
                    if (r.Byte() != 0)
                        throw new NotSupportedException("7z: external file names");
                    names = new string[n];
                    for (int i = 0; i < n; i++)
                        names[i] = r.WideString();
                }
                else if (type == 0x14)
                {
                    bool[] def = r.OptionalBitVector(n);
                    if (r.Byte() != 0)
                        throw new NotSupportedException("7z: external times");
                    for (int i = 0; i < n; i++)
                        if (def[i])
                            mtimes[i] = (long)r.UInt64();
                }
                else if (type == 0x15)
                {
                    attrDefined = r.OptionalBitVector(n);
                    if (r.Byte() != 0)
                        throw new NotSupportedException("7z: external attributes");
                    for (int i = 0; i < n; i++)
                        if (attrDefined[i])
                            attrs[i] = r.UInt32();
                }
                r.Pos = end;                     // every other property (creation / access times, padding, ...) is skipped
            }
            if (n > 0 && names == null)
                throw Bad("the file names are missing");
            int emptyIndex = 0, streamIndex = 0, folder = 0, inFolder = 0;
            for (int i = 0; i < n; i++)
            {
                var f = new SzFile { Name = names[i], MTime = mtimes[i] };
                if (emptyStream[i])
                {
                    bool isEmptyFile = emptyFile != null && emptyFile[emptyIndex];
                    f.IsAnti = anti != null && anti[emptyIndex];
                    f.IsDir = !isEmptyFile;
                    emptyIndex++;
                }
                else
                {
                    while (folder < main.Folders.Length && inFolder >= main.NumSub[folder])
                    {
                        folder++;
                        inFolder = 0;
                    }
                    if (folder >= main.Folders.Length || streamIndex >= main.SubSizes.Length)
                        throw Bad("more files than data");
                    f.HasStream = true;
                    f.Folder = folder;
                    f.Size = main.SubSizes[streamIndex];
                    f.HasCrc = main.SubHasCrc[streamIndex];
                    f.Crc = main.SubCrc[streamIndex];
                    inFolder++;
                    streamIndex++;
                }
                if (attrDefined[i] && (attrs[i] & 0x10) != 0 && !f.HasStream)
                    f.IsDir = true;
                list[i] = f;
            }
            files = list;
        }

        // -----------------------------------------------------------------------------------------------------------
        // Unpacking
        // -----------------------------------------------------------------------------------------------------------

        // The stream that gives the data of one coder output; its inputs are other coders' outputs or pack streams.
        Stream OpenOut(SzStreams si, SzFolder folder, int outIndex, List<IDisposable> owned, int depth)
        {
            if (depth > 32)
                throw Bad("the coders of a block are wired in a loop");
            int c = -1;
            for (int i = 0; i < folder.Coders.Length; i++)
                if (outIndex >= folder.FirstOut[i] && outIndex < folder.FirstOut[i] + folder.Coders[i].NumOut)
                    c = i;
            if (c < 0)
                throw Bad("a coder output that does not exist");
            SzCoder coder = folder.Coders[c];
            if (coder.NumOut != 1)
                throw new NotSupportedException("7z: a coder with several outputs");
            var ins = new Stream[coder.NumIn];
            for (int j = 0; j < ins.Length; j++)
            {
                int inIndex = folder.FirstIn[c] + j;
                int bind = Array.IndexOf(folder.BindIn, inIndex);
                if (bind >= 0)
                    ins[j] = OpenOut(si, folder, folder.BindOut[bind], owned, depth + 1);
                else
                {
                    int k = Array.IndexOf(folder.PackedIn, inIndex);
                    if (k < 0)
                        throw Bad("a coder input that gets no data");
                    int pack = folder.FirstPackStream + k;
                    var p = new SzPackStream(path, si.PackOffsets[pack], si.PackSizes[pack]);
                    owned.Add(p);
                    ins[j] = p;
                }
            }
            Stream result = MakeCoder(coder, ins, folder.OutSizes[outIndex]);
            owned.Add(result);
            return result;
        }

        static Stream MakeCoder(SzCoder coder, Stream[] ins, long outSize)
        {
            switch (coder.Method)
            {
                case 0x00:                       // Copy
                    return ins[0];
                case 0x030101:                   // LZMA
                    return new SzLzmaStream(ins[0], coder.Props, outSize);
                case 0x21:                       // LZMA2
                    return new SzLzma2Stream(ins[0], coder.Props, outSize);
                case 0x04:                       // BCJ (x86), as 7-Zip 9.x writes it as 03030103
                case 0x03030103:
                    return new SzBcjX86Stream(ins[0]);
                case 0x0303011B:                 // BCJ2: the main stream, the call and jump targets, the range coder's bits
                    if (ins.Length == 4)
                        return new SzBcj2Stream(ins, outSize);
                    break;
            }
            throw new NotSupportedException("7z: the method " + coder.Method.ToString("X") + " is not supported by the launcher's reader");
        }

        static string SafePath(string root, string name)
        {
            string full;
            try
            {
                full = Path.GetFullPath(Path.Combine(root, name.Replace('/', '\\')));
            }
            catch (Exception)
            {
                throw Bad("a file name that Windows cannot use: " + name);
            }
            if (!full.StartsWith(root.TrimEnd('\\') + "\\", StringComparison.OrdinalIgnoreCase))
                throw Bad("a file name that leads out of the target folder: " + name);
            return full;
        }

        static void SetTime(string target, long fileTime, bool directory)
        {
            if (fileTime <= 0)
                return;
            try
            {
                DateTime t = DateTime.FromFileTimeUtc(fileTime);
                if (directory)
                    Directory.SetLastWriteTimeUtc(target, t);
                else
                    File.SetLastWriteTimeUtc(target, t);
            }
            catch (Exception)
            {
            }
        }

        void Tick(int bytes)
        {
            done += bytes;
            if (cancelled != null && cancelled())
                throw new OperationCanceledException();
            if (progress != null)
                progress(done, total);
        }

        public int ExtractAll(string targetDir, Action<long, long> progress, Func<bool> cancelled)
        {
            this.progress = progress;
            this.cancelled = cancelled;
            string root = Path.GetFullPath(targetDir);
            Directory.CreateDirectory(root);
            done = 0;
            total = 0;
            foreach (SzFile f in files)
                if (f.HasStream)
                    total += f.Size;
            int count = 0;
            var dirTimes = new List<KeyValuePair<string, long>>();
            foreach (SzFile f in files)          // folders and empty files first
            {
                if (f.HasStream || f.IsAnti)
                    continue;
                string target = SafePath(root, f.Name);
                if (f.IsDir)
                {
                    Directory.CreateDirectory(target);
                    dirTimes.Add(new KeyValuePair<string, long>(target, f.MTime));
                }
                else
                {
                    Directory.CreateDirectory(Path.GetDirectoryName(target));
                    File.WriteAllBytes(target, new byte[0]);
                    SetTime(target, f.MTime, false);
                    count++;
                }
            }
            var buffer = new byte[1 << 18];
            for (int fi = 0; fi < main.Folders.Length; fi++)
            {
                var owned = new List<IDisposable>();
                try
                {
                    Stream stream = null;
                    foreach (SzFile f in files)  // the files of a block come out of it in the order of the list
                    {
                        if (!f.HasStream || f.Folder != fi)
                            continue;
                        if (stream == null)
                            stream = OpenOut(main, main.Folders[fi], main.Folders[fi].MainOut, owned, 0);
                        string target = SafePath(root, f.Name);
                        Directory.CreateDirectory(Path.GetDirectoryName(target));
                        uint crc = 0;
                        long left = f.Size;
                        using (var o = new FileStream(target, FileMode.Create, FileAccess.Write, FileShare.None, 1 << 16))
                            while (left > 0)
                            {
                                int got = stream.Read(buffer, 0, (int)Math.Min(buffer.Length, left));
                                if (got <= 0)
                                    throw Bad("the data ends early in " + f.Name);
                                crc = SzCrc32.Update(crc, buffer, 0, got);
                                o.Write(buffer, 0, got);
                                left -= got;
                                Tick(got);
                            }
                        if (f.HasCrc && crc != f.Crc)
                            throw Bad("the CRC of " + f.Name + " does not match: the data is damaged");
                        SetTime(target, f.MTime, false);
                        count++;
                    }
                }
                finally
                {
                    foreach (IDisposable d in owned)
                        d.Dispose();
                }
            }
            foreach (KeyValuePair<string, long> d in dirTimes)
                SetTime(d.Key, d.Value, true);
            if (progress != null)
                progress(total, total);
            return count;
        }
    }

    // ---------------------------------------------------------------------------------------------------------------
    // Streams
    // ---------------------------------------------------------------------------------------------------------------

    static class SzCrc32
    {
        static readonly uint[] table = Make();

        static uint[] Make()
        {
            var t = new uint[256];
            for (uint i = 0; i < 256; i++)
            {
                uint c = i;
                for (int k = 0; k < 8; k++)
                    c = (c & 1) != 0 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
                t[i] = c;
            }
            return t;
        }

        public static uint Update(uint crc, byte[] data, int offset, int count)
        {
            uint c = ~crc;
            uint[] t = table;
            for (int i = offset, end = offset + count; i < end; i++)
                c = t[(c ^ data[i]) & 0xFF] ^ (c >> 8);
            return ~c;
        }

        public static uint Of(byte[] data, int offset, int count)
        {
            return Update(0, data, offset, count);
        }
    }

    static class SzStream
    {
        // Reads until count bytes are there or the stream ends; returns how many came.
        public static int ReadSome(Stream s, byte[] buffer, int offset, int count)
        {
            int got = 0;
            while (got < count)
            {
                int n = s.Read(buffer, offset + got, count - got);
                if (n <= 0)
                    break;
                got += n;
            }
            return got;
        }

        public static void ReadAll(Stream s, byte[] buffer, int offset, int count)
        {
            if (ReadSome(s, buffer, offset, count) != count)
                throw new InvalidDataException("7z: the data ends early");
        }
    }

    abstract class SzReadStream : Stream     // a stream that only reads, forwards
    {
        readonly byte[] one = new byte[1];

        public override bool CanRead { get { return true; } }
        public override bool CanSeek { get { return false; } }
        public override bool CanWrite { get { return false; } }
        public override long Length { get { throw new NotSupportedException(); } }
        public override long Position { get { throw new NotSupportedException(); } set { throw new NotSupportedException(); } }

        public override void Flush()
        {
        }

        public override long Seek(long offset, SeekOrigin origin)
        {
            throw new NotSupportedException();
        }

        public override void SetLength(long value)
        {
            throw new NotSupportedException();
        }

        public override void Write(byte[] buffer, int offset, int count)
        {
            throw new NotSupportedException();
        }

        public override int ReadByte()
        {
            return Read(one, 0, 1) == 1 ? one[0] : -1;
        }
    }

    // A part of the archive file: each pack stream has a file handle of its own, so several can be read in turn.
    sealed class SzPackStream : SzReadStream
    {
        readonly FileStream file;
        long left;

        public SzPackStream(string path, long offset, long length)
        {
            file = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite, 1 << 16, FileOptions.SequentialScan);
            file.Position = offset;
            left = length;
        }

        public override int Read(byte[] buffer, int offset, int count)
        {
            if (left <= 0 || count <= 0)
                return 0;
            if (count > left)
                count = (int)left;
            int n = file.Read(buffer, offset, count);
            left -= n;
            return n;
        }

        protected override void Dispose(bool disposing)
        {
            if (disposing)
                file.Dispose();
            base.Dispose(disposing);
        }
    }

    // Bytes one at a time out of a stream (BCJ2 reads its four inputs like this).
    sealed class SzByteSource
    {
        readonly Stream s;
        readonly byte[] buf = new byte[1 << 16];
        int pos, end;

        public SzByteSource(Stream s)
        {
            this.s = s;
        }

        public int Next()                    // -1 at the end
        {
            if (pos == end)
            {
                end = s.Read(buf, 0, buf.Length);
                pos = 0;
                if (end <= 0)
                {
                    end = 0;
                    return -1;
                }
            }
            return buf[pos++];
        }
    }

    // The LZMA decoder (LzmaDec.c): a window of the last bytes, the probability model, the range decoder. The data comes
    // out of Read; one symbol can make up to 273 bytes, so a match that does not fit what was asked for waits (pendLen).
    abstract class SzLzStream : SzReadStream
    {
        const uint TopValue = 1u << 24;
        const uint BitModelTotal = 1u << 11;
        const int MoveBits = 5;
        const int NumStates = 12;
        const int MaxBlock = 1 << 18;

        // where each part of the model sits in probs
        const int IsMatch = 0;                                   // [state << 4 | posState]
        const int IsRep = IsMatch + (NumStates << 4);
        const int IsRepG0 = IsRep + NumStates;
        const int IsRepG1 = IsRepG0 + NumStates;
        const int IsRepG2 = IsRepG1 + NumStates;
        const int IsRep0Long = IsRepG2 + NumStates;              // [state << 4 | posState]
        const int PosSlot = IsRep0Long + (NumStates << 4);       // four trees of 6 bits
        const int SpecPos = PosSlot + 4 * 64;
        const int Align = SpecPos + 115;
        const int LenCoder = Align + 16;
        const int RepLenCoder = LenCoder + 514;
        const int Literal = RepLenCoder + 514;
        const int LenChoice = 0, LenChoice2 = 1, LenLow = 2, LenMid = LenLow + (16 << 3), LenHigh = LenMid + (16 << 3);

        byte[] win;
        int winSize, winPos, flushPos;       // the window; where the next byte goes; how much of it the reader has taken
        long processed;                      // bytes made since the last dictionary reset

        ushort[] probs;
        int lc, lp, pb, lpMask, pbMask, state, pendLen;
        uint rep0, rep1, rep2, rep3;

        protected readonly byte[] InBuf = new byte[1 << 16];     // LZMA2 puts a whole chunk here, LZMA refills it from its stream
        int inPos, inEnd;
        Stream inSource;
        uint range, code;

        protected static Exception Corrupt(string what)
        {
            return new InvalidDataException("7z: the LZMA data is damaged (" + what + ")");
        }

        protected void InitWindow(long dictSize, long outSize)
        {
            long size = Math.Min(dictSize, outSize);   // what is never reached back to needs no room
            if (size < 4096)
                size = 4096;
            if (size > (1L << 30))
                throw new NotSupportedException("7z: the LZMA dictionary is too big for the launcher's reader");
            win = new byte[(int)size];
            winSize = (int)size;
        }

        protected void SetProps(int lcBits, int lpBits, int pbBits)
        {
            lc = lcBits;
            lp = lpBits;
            pb = pbBits;
            lpMask = (1 << lp) - 1;
            pbMask = (1 << pb) - 1;
            int size = Literal + (0x300 << (lc + lp));
            if (probs == null || probs.Length != size)
                probs = new ushort[size];
        }

        protected void ResetState()
        {
            for (int i = 0; i < probs.Length; i++)
                probs[i] = (ushort)(BitModelTotal >> 1);
            state = 0;
            rep0 = rep1 = rep2 = rep3 = 0;
            pendLen = 0;
        }

        protected void ResetDict()
        {
            processed = 0;
        }

        protected int PendingLength { get { return pendLen; } }

        protected void AddUncompressed(Stream from, int count)   // LZMA2's stored chunks go straight into the window
        {
            SzStream.ReadAll(from, win, winPos, count);
            winPos += count;
            processed += count;
        }

        protected int WinPos { get { return winPos; } }

        // ---- range decoder ----

        protected void RcFromStream(Stream s)
        {
            inSource = s;
            inPos = inEnd = 0;
        }

        protected void RcFromChunk(int count)
        {
            inSource = null;
            inPos = 0;
            inEnd = count;
        }

        protected void RcInit()
        {
            if (NextByte() != 0)
                throw Corrupt("the range coder");
            range = 0xFFFFFFFF;
            code = 0;
            for (int i = 0; i < 4; i++)
                code = (code << 8) | (uint)NextByte();
        }

        int NextByte()
        {
            if (inPos == inEnd)
                Refill();
            return InBuf[inPos++];
        }

        void Refill()
        {
            if (inSource == null)
                throw Corrupt("a chunk ends early");
            int n = inSource.Read(InBuf, 0, InBuf.Length);
            if (n <= 0)
                throw Corrupt("the stream ends early");
            inPos = 0;
            inEnd = n;
        }

        [MethodImpl(MethodImplOptions.AggressiveInlining)]
        int Bit(int index)
        {
            uint p = probs[index];
            if (range < TopValue)
            {
                if (inPos == inEnd)
                    Refill();
                range <<= 8;
                code = (code << 8) | InBuf[inPos++];
            }
            uint bound = (range >> 11) * p;
            if (code < bound)
            {
                range = bound;
                probs[index] = (ushort)(p + ((BitModelTotal - p) >> MoveBits));
                return 0;
            }
            range -= bound;
            code -= bound;
            probs[index] = (ushort)(p - (p >> MoveBits));
            return 1;
        }

        int BitTree(int baseIndex, int bits)
        {
            int m = 1;
            for (int i = 0; i < bits; i++)
                m = (m << 1) + Bit(baseIndex + m);
            return m - (1 << bits);
        }

        int BitTreeReverse(int baseIndex, int bits)
        {
            int m = 1, symbol = 0;
            for (int i = 0; i < bits; i++)
            {
                int b = Bit(baseIndex + m);
                m = (m << 1) + b;
                symbol |= b << i;
            }
            return symbol;
        }

        uint DirectBits(int count)
        {
            uint result = 0;
            for (int i = 0; i < count; i++)
            {
                if (range < TopValue)
                {
                    if (inPos == inEnd)
                        Refill();
                    range <<= 8;
                    code = (code << 8) | InBuf[inPos++];
                }
                range >>= 1;
                uint t = (code - range) >> 31;       // 1 when the bit is 0
                code -= range & (t - 1);
                result = (result << 1) | (1 - t);
            }
            return result;
        }

        int DecodeLen(int coder, int posState)
        {
            if (Bit(coder + LenChoice) == 0)
                return BitTree(coder + LenLow + (posState << 3), 3);
            if (Bit(coder + LenChoice2) == 0)
                return 8 + BitTree(coder + LenMid + (posState << 3), 3);
            return 16 + BitTree(coder + LenHigh, 8);
        }

        uint DecodeDistance(int len)             // len: the length less the minimum of 2
        {
            int slot = BitTree(PosSlot + ((len < 4 ? len : 3) << 6), 6);
            if (slot < 4)
                return (uint)slot;
            int direct = (slot >> 1) - 1;
            uint dist = (uint)(2 | (slot & 1)) << direct;
            if (slot < 14)
                return dist + (uint)BitTreeReverse(SpecPos + (int)dist - slot - 1, direct);
            dist += DirectBits(direct - 4) << 4;
            return dist + (uint)BitTreeReverse(Align, 4);
        }

        void CopyMatch(int limit)
        {
            int n = limit - winPos;
            if (n > pendLen)
                n = pendLen;
            int src = winPos - (int)rep0 - 1;
            if (src < 0)
                src += winSize;
            int dst = winPos;
            byte[] w = win;
            for (int i = 0; i < n; i++)
            {
                w[dst++] = w[src++];
                if (src == winSize)
                    src = 0;
            }
            winPos = dst;
            processed += n;
            pendLen -= n;
        }

        // Makes bytes until the window is filled to limit (limit <= winSize); true when the stream's end marker came first.
        protected bool Decode(int limit)
        {
            if (pendLen > 0)
                CopyMatch(limit);
            while (winPos < limit)
            {
                int posState = (int)processed & pbMask;
                if (Bit(IsMatch + (state << 4) + posState) == 0)
                {
                    int prev = processed == 0 ? 0 : win[(winPos == 0 ? winSize : winPos) - 1];
                    int lit = Literal + 0x300 * ((((int)processed & lpMask) << lc) + (prev >> (8 - lc)));
                    int symbol = 1;
                    if (state >= 7)
                    {
                        int src = winPos - (int)rep0 - 1;
                        if (src < 0)
                            src += winSize;
                        int matchByte = win[src];
                        do
                        {
                            int matchBit = (matchByte >> 7) & 1;
                            matchByte <<= 1;
                            int bit = Bit(lit + ((1 + matchBit) << 8) + symbol);
                            symbol = (symbol << 1) | bit;
                            if (matchBit != bit)
                                break;
                        }
                        while (symbol < 0x100);
                    }
                    while (symbol < 0x100)
                        symbol = (symbol << 1) | Bit(lit + symbol);
                    win[winPos++] = (byte)symbol;
                    processed++;
                    state = state < 4 ? 0 : (state < 10 ? state - 3 : state - 6);
                    continue;
                }
                int len;
                if (Bit(IsRep + state) == 0)
                {
                    rep3 = rep2;
                    rep2 = rep1;
                    rep1 = rep0;
                    len = DecodeLen(LenCoder, posState);
                    state = state < 7 ? 7 : 10;
                    rep0 = DecodeDistance(len);
                    if (rep0 == 0xFFFFFFFF)
                        return true;
                }
                else
                {
                    if (processed == 0)
                        throw Corrupt("a repeat before any data");
                    if (Bit(IsRepG0 + state) == 0)
                    {
                        if (Bit(IsRep0Long + (state << 4) + posState) == 0)
                        {
                            state = state < 7 ? 9 : 11;          // a repeat of one byte
                            int src = winPos - (int)rep0 - 1;
                            if (src < 0)
                                src += winSize;
                            win[winPos++] = win[src];
                            processed++;
                            continue;
                        }
                    }
                    else
                    {
                        uint dist;
                        if (Bit(IsRepG1 + state) == 0)
                            dist = rep1;
                        else
                        {
                            if (Bit(IsRepG2 + state) == 0)
                                dist = rep2;
                            else
                            {
                                dist = rep3;
                                rep3 = rep2;
                            }
                            rep2 = rep1;
                        }
                        rep1 = rep0;
                        rep0 = dist;
                    }
                    len = DecodeLen(RepLenCoder, posState);
                    state = state < 7 ? 8 : 11;
                }
                if ((long)rep0 >= processed || rep0 >= (uint)winSize)
                    throw Corrupt("a match reaches back too far");
                pendLen = len + 2;
                CopyMatch(limit);
            }
            return false;
        }

        // Puts up to max more bytes at winPos (never past the window's end) and says how many; 0 = the end of the data.
        protected abstract int Produce(int max);

        public override int Read(byte[] buffer, int offset, int count)
        {
            int total = 0;
            while (count > 0)
            {
                if (flushPos == winPos)          // everything made so far is taken: make more
                {
                    if (winPos == winSize)
                        winPos = flushPos = 0;
                    int room = winSize - winPos;
                    if (room > MaxBlock)
                        room = MaxBlock;
                    if (Produce(room) <= 0)
                        break;
                }
                int n = winPos - flushPos;
                if (n > count)
                    n = count;
                Buffer.BlockCopy(win, flushPos, buffer, offset, n);
                flushPos += n;
                offset += n;
                count -= n;
                total += n;
            }
            return total;
        }
    }

    // LZMA as 7z holds it: the properties (lc/lp/pb and the dictionary size) are 5 bytes, the size of the result is known.
    sealed class SzLzmaStream : SzLzStream
    {
        long remaining;
        bool started;

        public SzLzmaStream(Stream input, byte[] props, long outSize)
        {
            if (props == null || props.Length < 5 || props[0] >= 9 * 5 * 5)
                throw Corrupt("the properties");
            int d = props[0];
            int lcBits = d % 9;
            d /= 9;
            int lpBits = d % 5;
            int pbBits = d / 5;
            long dict = props[1] | ((long)props[2] << 8) | ((long)props[3] << 16) | ((long)props[4] << 24);
            InitWindow(dict, outSize);
            SetProps(lcBits, lpBits, pbBits);
            ResetState();
            RcFromStream(input);
            remaining = outSize;
        }

        protected override int Produce(int max)
        {
            if (remaining <= 0)
                return 0;
            if (!started)
            {
                RcInit();
                started = true;
            }
            if (max > remaining)
                max = (int)remaining;
            int start = WinPos;
            if (Decode(start + max))
                throw Corrupt("the end marker comes before the data is complete");
            int made = WinPos - start;
            remaining -= made;
            return made;
        }
    }

    // LZMA2 (Lzma2Dec.c): chunks, each stored or LZMA-packed, with resets of the dictionary, the state and the properties.
    sealed class SzLzma2Stream : SzLzStream
    {
        readonly Stream input;
        long remaining;
        int chunkLeft;
        bool chunkIsLzma, needDictReset = true, needProps = true;

        public SzLzma2Stream(Stream input, byte[] props, long outSize)
        {
            if (props == null || props.Length < 1 || props[0] > 40)
                throw Corrupt("the LZMA2 properties");
            long dict = props[0] == 40 ? 0xFFFFFFFFL : (long)(2 | (props[0] & 1)) << (props[0] / 2 + 11);
            InitWindow(dict, outSize);
            this.input = input;
            remaining = outSize;
        }

        int B()
        {
            int v = input.ReadByte();
            if (v < 0)
                throw Corrupt("the stream ends early");
            return v;
        }

        void NextChunk()
        {
            int control = B();
            if (control == 0)
                throw Corrupt("the stream ends before the data is complete");
            if (control >= 0x80)
            {
                int unpacked = ((control & 0x1F) << 16) + (B() << 8) + B() + 1;
                int packed = (B() << 8) + B() + 1;
                int mode = (control >> 5) & 3;       // 1: new state, 2: and new properties, 3: and a new dictionary
                if (mode == 3)
                {
                    ResetDict();
                    needDictReset = false;
                }
                else if (needDictReset)
                    throw Corrupt("the first chunk does not reset the dictionary");
                if (mode >= 2)
                {
                    int p = B();
                    if (p >= 9 * 5 * 5)
                        throw Corrupt("the chunk's properties");
                    int lcBits = p % 9;
                    p /= 9;
                    int lpBits = p % 5;
                    int pbBits = p / 5;
                    if (lcBits + lpBits > 4)
                        throw Corrupt("the chunk's properties");
                    SetProps(lcBits, lpBits, pbBits);
                    needProps = false;
                }
                else if (needProps)
                    throw Corrupt("a chunk without properties");
                if (mode >= 1)
                    ResetState();
                SzStream.ReadAll(input, InBuf, 0, packed);
                RcFromChunk(packed);
                RcInit();
                chunkIsLzma = true;
                chunkLeft = unpacked;
            }
            else if (control == 1 || control == 2)   // stored: 1 resets the dictionary
            {
                if (control == 1)
                {
                    ResetDict();
                    needDictReset = false;
                    needProps = true;
                }
                else if (needDictReset)
                    throw Corrupt("the first chunk does not reset the dictionary");
                chunkIsLzma = false;
                chunkLeft = (B() << 8) + B() + 1;
            }
            else
                throw Corrupt("a chunk of an unknown kind");
        }

        protected override int Produce(int max)
        {
            if (remaining <= 0)
                return 0;
            if (max > remaining)
                max = (int)remaining;
            while (chunkLeft == 0)
                NextChunk();
            int n = Math.Min(max, chunkLeft);
            if (chunkIsLzma)
            {
                if (Decode(WinPos + n))
                    throw Corrupt("an end marker inside an LZMA2 chunk");
                if (n == chunkLeft && PendingLength != 0)
                    throw Corrupt("a match runs over the end of its chunk");
            }
            else
                AddUncompressed(input, n);
            chunkLeft -= n;
            remaining -= n;
            return n;
        }
    }

    // The x86 filter (Bra86.c): calls and jumps were turned into absolute addresses before packing; here they are turned back.
    sealed class SzBcjX86Stream : SzReadStream
    {
        readonly Stream input;
        readonly byte[] b = new byte[1 << 16];
        int pos, converted, filled;          // [pos, converted) is ready to hand out, [converted, filled) waits for more data
        uint ip, mask;
        bool ended;

        static readonly bool[] MaskToAllowed = { true, true, true, false, true, false, false, false };
        static readonly int[] MaskToBit = { 0, 1, 2, 2, 3, 3, 3, 3 };

        public SzBcjX86Stream(Stream input)
        {
            this.input = input;
        }

        static bool IsMsByte(byte x)
        {
            return x == 0 || x == 0xFF;
        }

        static int Convert(byte[] data, int size, uint ip, ref uint state)
        {
            int bufferPos = 0;
            uint prevMask = state & 7;
            if (size < 5)
                return 0;
            ip += 5;
            int prevPos = -1;
            while (true)
            {
                int p = bufferPos;
                int limit = size - 4;
                for (; p < limit; p++)
                    if ((data[p] & 0xFE) == 0xE8)
                        break;
                bufferPos = p;
                if (p >= limit)
                    break;
                prevPos = bufferPos - prevPos;
                if (prevPos > 3)
                    prevMask = 0;
                else
                {
                    prevMask = (prevMask << (prevPos - 1)) & 7;
                    if (prevMask != 0)
                    {
                        byte x = data[p + 4 - MaskToBit[prevMask]];
                        if (!MaskToAllowed[prevMask] || IsMsByte(x))
                        {
                            prevPos = bufferPos;
                            prevMask = ((prevMask << 1) & 7) | 1;
                            bufferPos++;
                            continue;
                        }
                    }
                }
                prevPos = bufferPos;
                if (IsMsByte(data[p + 4]))
                {
                    uint src = ((uint)data[p + 4] << 24) | ((uint)data[p + 3] << 16) | ((uint)data[p + 2] << 8) | data[p + 1];
                    uint dest;
                    while (true)
                    {
                        dest = src - (ip + (uint)bufferPos);
                        if (prevMask == 0)
                            break;
                        int index = MaskToBit[prevMask] * 8;
                        byte x = (byte)(dest >> (24 - index));
                        if (!IsMsByte(x))
                            break;
                        src = dest ^ ((1u << (32 - index)) - 1);
                    }
                    data[p + 4] = (byte)(~(((dest >> 24) & 1) - 1));
                    data[p + 3] = (byte)(dest >> 16);
                    data[p + 2] = (byte)(dest >> 8);
                    data[p + 1] = (byte)dest;
                    bufferPos += 5;
                }
                else
                {
                    prevMask = ((prevMask << 1) & 7) | 1;
                    bufferPos++;
                }
            }
            prevPos = bufferPos - prevPos;
            state = prevPos > 3 ? 0 : ((prevMask << (prevPos - 1)) & 7);
            return bufferPos;
        }

        public override int Read(byte[] buffer, int offset, int count)
        {
            int total = 0;
            while (count > 0)
            {
                if (pos < converted)
                {
                    int n = Math.Min(count, converted - pos);
                    Buffer.BlockCopy(b, pos, buffer, offset, n);
                    pos += n;
                    offset += n;
                    count -= n;
                    total += n;
                    continue;
                }
                int rest = filled - converted;   // the last bytes that could not be judged yet move to the front
                Buffer.BlockCopy(b, converted, b, 0, rest);
                pos = converted = 0;
                filled = rest;
                if (!ended)
                {
                    int n = input.Read(b, filled, b.Length - filled);
                    if (n <= 0)
                        ended = true;
                    else
                        filled += n;
                }
                if (filled == 0)
                    break;
                if (ended)
                {
                    Convert(b, filled, ip, ref mask);
                    converted = filled;          // what is left at the very end stays as it is
                    ip += (uint)filled;
                }
                else
                {
                    int done = Convert(b, filled, ip, ref mask);
                    converted = done;
                    ip += (uint)done;
                }
            }
            return total;
        }
    }

    // BCJ2 (Bcj2.c): the main stream with the 4-byte targets of calls (E8) and jumps (E9, 0F 8x) taken out into two streams
    // of their own; a range coder tells for each such opcode whether its target was taken out.
    sealed class SzBcj2Stream : SzReadStream
    {
        readonly SzByteSource main, call, jump, rc;
        readonly ushort[] probs = new ushort[258];
        readonly long outSize;
        readonly byte[] pend = new byte[4];
        int pendPos, pendCount;
        long outPos;
        int prev;
        uint range, code;

        public SzBcj2Stream(Stream[] ins, long outSize)
        {
            main = new SzByteSource(ins[0]);
            call = new SzByteSource(ins[1]);
            jump = new SzByteSource(ins[2]);
            rc = new SzByteSource(ins[3]);
            this.outSize = outSize;
            for (int i = 0; i < probs.Length; i++)
                probs[i] = 1 << 10;
            range = 0xFFFFFFFF;
            for (int i = 0; i < 5; i++)
                code = (code << 8) | Take(rc);
        }

        static uint Take(SzByteSource s)
        {
            int v = s.Next();
            if (v < 0)
                throw new InvalidDataException("7z: the BCJ2 data ends early");
            return (uint)v;
        }

        public override int Read(byte[] buffer, int offset, int count)
        {
            int total = 0;
            while (total < count)
            {
                if (pendCount > 0)
                {
                    buffer[offset + total++] = pend[pendPos++];
                    pendCount--;
                    continue;
                }
                if (outPos >= outSize)
                    break;
                int b = main.Next();
                if (b < 0)
                    break;
                buffer[offset + total++] = (byte)b;
                outPos++;
                bool isJump = (b & 0xFE) == 0xE8 || (prev == 0x0F && (b & 0xF0) == 0x80);
                if (!isJump)
                {
                    prev = b;
                    continue;
                }
                if (outPos >= outSize)
                    break;                       // an opcode that ends the data has no flag
                int index = b == 0xE8 ? prev : (b == 0xE9 ? 256 : 257);
                uint p = probs[index];
                if (range < (1u << 24))
                {
                    range <<= 8;
                    code = (code << 8) | Take(rc);
                }
                uint bound = (range >> 11) * p;
                if (code < bound)
                {
                    range = bound;
                    probs[index] = (ushort)(p + ((2048u - p) >> 5));
                    prev = b;
                    continue;
                }
                range -= bound;
                code -= bound;
                probs[index] = (ushort)(p - (p >> 5));
                SzByteSource from = b == 0xE8 ? call : jump;
                uint src = (Take(from) << 24) | (Take(from) << 16) | (Take(from) << 8) | Take(from);
                uint dest = src - ((uint)outPos + 4);
                pend[0] = (byte)dest;
                pend[1] = (byte)(dest >> 8);
                pend[2] = (byte)(dest >> 16);
                pend[3] = (byte)(dest >> 24);
                pendPos = 0;
                pendCount = (int)Math.Min(4L, outSize - outPos);
                outPos += 4;
                prev = pend[3];
            }
            return total;
        }
    }
}
