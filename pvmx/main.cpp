#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <cstdint>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "FileSystem.h"

#define long int64_t

using ulong  = uint64_t;
using uint   = uint32_t;
using ushort = uint16_t;
using byte   = int8_t;
using ubyte  = uint8_t;

static_assert(sizeof(ulong) == sizeof(long), "long size mismatch");
static_assert(sizeof(long) == 8, "long size mismatch");
static_assert(sizeof(uint) == sizeof(int), "int size mismatch");
static_assert(sizeof(ushort) == sizeof(short), "short size mismatch");

constexpr int PVMX_FOURCC = 'XMVP';
static const ubyte PVMX_VERSION = 1;

struct TexPackEntry
{
	uint32_t globalIndex;
	std::string name;
	uint32_t width, height;
};

struct DictionaryEntry : TexPackEntry
{
	ulong offset;
	ulong size;
};

namespace DictionaryField
{
	enum _ : ubyte
	{
		None,
		// 32-bit integer global index
		GlobalIndex,
		// Null-terminated file name
		Name,
		// Two 32-bit integers defining width and height
		Dimensions,
	};

	static_assert(sizeof(None) == sizeof(ubyte), "DictionaryField size mismatch");
}

static void usage()
{
	using namespace std;

	cout
		<< "Usage:" << endl
		<< "\t-c, --create     Create an archive using the given texture pack index."        << endl
		<< "\t-e, --extract    Extract an archive."                                          << endl
		<< "\t-o, --output     Output file for creation or output directory for extraction." << endl;
}

#pragma region convenience

template <typename T>
auto& write_t(std::ofstream& file, const T& data)
{
	return file.write(reinterpret_cast<const char*>(&data), sizeof(T));
}

template <typename T>
auto& read_t(std::ifstream& file, T& data)
{
	return file.read(reinterpret_cast<char*>(&data), sizeof(T));
}

void read_cstr(std::ifstream& file, std::string& out)
{
	std::stringstream buffer;

	while (true)
	{
		char c;
		read_t(file, c);
		if (c == '\0')
		{
			break;
		}

		buffer.put(c);
	}

	out = buffer.str();
}

#pragma endregion

static void create_archive(const std::string& input_path, const std::string& output_path)
{
	using namespace std;

	string _input_path;

	if (DirectoryExists(input_path))
	{
		_input_path = move(CombinePath(input_path, "index.txt"));
	}
	else
	{
		_input_path = move(input_path);
	}

	if (!FileExists(_input_path))
	{
		cout << "File not found: " << input_path << endl;
		return;
	}

	ifstream index_file(_input_path);

	if (!index_file.is_open())
	{
		cout << "Failed to open index file: " << _input_path << endl;
		return;
	}

	string _output_path;

	if (output_path.empty())
	{
		auto path = move(CombinePath(GetWorkingDirectory(), _input_path));
		auto dir = move(GetDirectory(path));
		_output_path = move(CombinePath(GetDirectory(dir), GetBaseName(dir) + ".pvmx"));
	}
	else
	{
		_output_path = move(output_path);
	}

	ofstream out_file(_output_path, ios::out | ios::binary);

	if (!out_file.is_open())
	{
		cout << "Failed to create file: " << output_path << endl;
		return;
	}

	vector<string> files;
	vector<size_t> dict_offsets;

	// FourCC
	write_t(out_file, PVMX_FOURCC);
	// Archive version
	write_t(out_file, PVMX_VERSION);

	uint line_number = 0;
	string line;

	while (!index_file.eof())
	{
		try
		{
			++line_number;
			getline(index_file, line);

			if (line.length() == 0 || line[0] == '#')
			{
				continue;
			}

			size_t comma = line.find(',');

			if (comma < 1 && comma != line.npos)
			{
				printf("Invalid texture index entry on line %u (missing comma?)\n", line_number);
				return;
			}

			uint width = 0;
			uint height = 0;

			uint gbix = stoul(line.substr(0, comma));
			auto name = line.substr(comma + 1);

			comma = name.find(',');

			// check for an additional texture dimensions field
			if (comma != name.npos && comma > 0)
			{
				auto tmp = name;
				name = tmp.substr(0, comma);

				auto dimensions = tmp.substr(++comma);
				size_t separator = dimensions.find('x');

				// If no 'x' separator is found, try capital
				if (!separator || separator == dimensions.npos)
				{
					separator = dimensions.find('X');
				}

				if (!separator || separator == dimensions.npos)
				{
					printf("Invalid format for texture dimensions on line %u: %s\n",
						line_number, dimensions.c_str());
					return;
				}

				width = stoul(dimensions.substr(0, separator));
				height = stoul(dimensions.substr(++separator));
			}

			files.push_back(name);

			write_t(out_file, DictionaryField::GlobalIndex);
			write_t(out_file, gbix);

			write_t(out_file, DictionaryField::Name);
			out_file << name << '\0';

			if (width || height)
			{
				write_t(out_file, DictionaryField::Dimensions);
				write_t(out_file, width);
				write_t(out_file, height);
			}

			write_t(out_file, DictionaryField::None);

			dict_offsets.push_back(out_file.tellp());

			// Offset (to be populated later)
			write_t(out_file, static_cast<ulong>(0));
			// Size (to be populated later)
			write_t(out_file, static_cast<ulong>(0));
		}
		catch (exception& exception)
		{
			printf("An exception occurred while parsing texture index on line %u: %s\n", line_number, exception.what());
			return;
		}
	}
	
	// Dictionary element starting with 0 marks the end of dictionary.
	write_t(out_file, DictionaryField::None);

	// Tracks file offsets (first) and sizes (second)
	unordered_map<string, pair<size_t, size_t>> file_meta;

	string dir = GetDirectory(_input_path);

	char buffer[4096] = {};
	static_assert(sizeof(buffer) == 4096, "nope");

	// Write file data to the data section of the archive
	for (size_t i = 0; i < files.size(); i++)
	{
		auto it = file_meta.find(files[i]);
		if (it != file_meta.end())
		{
			continue;
		}

		auto path = move(CombinePath(dir, files[i]));
		ifstream in_file(path, ios::in | ios::binary | ios::ate);

		if (!in_file.is_open())
		{
			cout << "Unable to open file: " << path << endl;
			return;
		}

		auto size = static_cast<size_t>(in_file.tellg());
		file_meta[files[i]] = make_pair(static_cast<size_t>(out_file.tellp()), size);
		in_file.seekg(0);

		while (!in_file.eof())
		{
			auto start = static_cast<size_t>(in_file.tellg());

			if (start == size)
			{
				break;
			}

			in_file.read(buffer, min(sizeof(buffer), size - start));
			auto end = static_cast<size_t>(in_file.tellg());
			auto delta = end - start;
			out_file.write(buffer, delta);
		}
	}

	// Write file offsets and sizes to the dictionary
	for (size_t i = 0; i < files.size(); i++)
	{
		out_file.seekp(dict_offsets[i]);

		auto& pair = file_meta[files[i]];
		write_t(out_file, pair.first); // offset
		write_t(out_file, pair.second); // size
	}
}

static void extract_archive(const std::string& input_path, const std::string& output_path)
{
	using namespace std;

	ifstream in_file(input_path, ios::binary | ios::in);

	if (!in_file.is_open())
	{
		cout << "Unable to open input file: " << input_path << endl;
		return;
	}

	string _output_path;

	if (output_path.empty())
	{
		auto path = move(CombinePath(GetWorkingDirectory(), input_path));
		auto dir  = move(GetDirectory(path));
		auto name = move(GetBaseName(path));
		StripExtension(name);

		_output_path = move(CombinePath(dir, name));
	}
	else
	{
		_output_path = move(output_path);
	}

	if (!DirectoryExists(_output_path))
	{
		if (!CreateDirectoryA(_output_path.c_str(), nullptr))
		{
			cout << "Unable to create output directory: " << _output_path << endl;
			return;
		}
	}

	auto index_path = CombinePath(_output_path, "index.txt");
	ofstream index_file(index_path);

	if (!index_file.is_open())
	{
		cout << "Unable to create index file: " << index_path << endl;
		return;
	}

	int fourcc;
	ubyte version;

	read_t(in_file, fourcc);

	if (fourcc != PVMX_FOURCC)
	{
		cout << "File is not a PVMX archive." << endl;
		return;
	}

	read_t(in_file, version);

	if (version != PVMX_VERSION)
	{
		cout << "Incorrect PVMX archive version." << endl;
		return;
	}

	ubyte type = 0;
	vector<DictionaryEntry> entries;

	for (read_t(in_file, type); type != DictionaryField::None; read_t(in_file, type))
	{
		DictionaryEntry entry = {};

		while (type != DictionaryField::None)
		{
			switch (type)
			{
				case DictionaryField::GlobalIndex:
					read_t(in_file, entry.globalIndex);
					break;

				case DictionaryField::Name:
					read_cstr(in_file, entry.name);
					break;

				case DictionaryField::Dimensions:
					read_t(in_file, entry.width);
					read_t(in_file, entry.height);
					break;

				default:
					break;
			}

			read_t(in_file, type);
		}

		read_t(in_file, entry.offset);
		read_t(in_file, entry.size);

		entries.push_back(entry);

		int n = 0;
		if (entry.globalIndex)
		{
			index_file << entry.globalIndex;
			++n;
		}

		if (!entry.name.empty())
		{
			if (n++)
			{
				index_file << ',';
			}

			index_file << entry.name;
		}

		if (entry.width || entry.height)
		{
			if (n)
			{
				index_file << ',';
			}

			index_file << entry.width << 'x' << entry.height;
		}

		index_file << endl;
	}

	char buffer[4096] = {};
	static_assert(sizeof(buffer) == 4096, "nope");

	for (auto& i : entries)
	{
		cout << "Extracting: " << i.name << endl;

		auto path = move(CombinePath(_output_path, i.name));
		ofstream out_file(path, ios::binary | ios::out);
		if (!out_file.is_open())
		{
			cout << "Unable to open output file: " << path << endl;
			return;
		}

		in_file.seekg(i.offset);
		auto position = static_cast<ulong>(in_file.tellg());
		const auto end = i.offset + i.size;

		while (position < end)
		{
			auto start = position;
			in_file.read(buffer, min(static_cast<ulong>(sizeof(buffer)), end - start));
			position = static_cast<ulong>(in_file.tellg());
			auto delta = position - start;
			out_file.write(buffer, delta);
		}
	}
}

int main(int argc, char** argv)
{
	using namespace std;

	if (argc < 2)
	{
		usage();
		return 0;
	}

	bool create = false;
	bool extract = false;

	string input_path;
	string output_path;

	for (size_t i = 1; i < argc; i++)
	{
		auto arg = argv[i];
		if (!_strcmpi("--help", arg) || !_strcmpi("-h", arg) || !_strcmpi("-?", arg))
		{
			usage();
			return 0;
		}

		if (!_strcmpi("--create", arg) || !_strcmpi("-c", arg))
		{
			if (++i >= argc)
			{
				throw;
			}

			create = true;
			input_path = argv[i];
			continue;
		}

		if (!_strcmpi("--extract", arg) || !_strcmpi("-e", arg))
		{
			if (++i >= argc)
			{
				throw;
			}

			extract = true;
			input_path = argv[i];
			continue;
		}

		if (!_strcmpi("--output", arg) || !_strcmpi("-o", arg))
		{
			if (++i >= argc)
			{
				throw;
			}

			output_path = argv[i];
		}
	}

	if (input_path.empty())
	{
		cout << "Input path cannot be empty." << endl;
		usage();
		return -1;
	}

	if (create)
	{
		create_archive(input_path, output_path);
	}
	else if (extract)
	{
		extract_archive(input_path, output_path);
	}

	return 0;
}
