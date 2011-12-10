def string_table_entry (offset):
  return ('ptr', '+ + PointerToSymbolTable * NumberOfSymbols 18 %s' % offset, ('scalar', 'cstr', '%s'))

def secname(value):
  if value[0] == '/':
    return string_table_entry(value[1:].rstrip('\0'))
  else:
    return '%s'

def symname(value):
  parts = struct.unpack("<2L", value)
  if parts[0] == 0:
    return string_table_entry(parts[1])
  else:
    return '%s'

file = [('struct', 'header', '!Header', [
  ('MachineType', ('enum', '<H', '0x%X', {
    0x0:    'IMAGE_FILE_MACHINE_UNKNOWN',
    0x1d3:  'IMAGE_FILE_MACHINE_AM33',
    0x8664: 'IMAGE_FILE_MACHINE_AMD64',
    0x1c0:  'IMAGE_FILE_MACHINE_ARM',
    0xebc:  'IMAGE_FILE_MACHINE_EBC',
    0x14c:  'IMAGE_FILE_MACHINE_I386',
    0x200:  'IMAGE_FILE_MACHINE_IA64',
    0x904:  'IMAGE_FILE_MACHINE_M32R',
    0x266:  'IMAGE_FILE_MACHINE_MIPS16',
    0x366:  'IMAGE_FILE_MACHINE_MIPSFPU',
    0x466:  'IMAGE_FILE_MACHINE_MIPSFPU16',
    0x1f0:  'IMAGE_FILE_MACHINE_POWERPC',
    0x1f1:  'IMAGE_FILE_MACHINE_POWERPCFP',
    0x166:  'IMAGE_FILE_MACHINE_R4000',
    0x1a2:  'IMAGE_FILE_MACHINE_SH3',
    0x1a3:  'IMAGE_FILE_MACHINE_SH3DSP',
    0x1a6:  'IMAGE_FILE_MACHINE_SH4',
    0x1a8:  'IMAGE_FILE_MACHINE_SH5',
    0x1c2:  'IMAGE_FILE_MACHINE_THUMB',
    0x169:  'IMAGE_FILE_MACHINE_WCEMIPSV2',
  })),
  ('NumberOfSections',     ('scalar',  '<H', '%d')),
  ('TimeDateStamp',        ('scalar',  '<L', '%d')),
  ('PointerToSymbolTable', ('scalar',  '<L', '0x%0X')),
  ('NumberOfSymbols',      ('scalar',  '<L', '%d')),
  ('SizeOfOptionalHeader', ('scalar',  '<H', '%d')),
  ('Characteristics',      ('flags',   '<H', '0x%x', [
    (0x0001,      'IMAGE_FILE_RELOCS_STRIPPED',         ),
    (0x0002,      'IMAGE_FILE_EXECUTABLE_IMAGE',        ),
    (0x0004,      'IMAGE_FILE_LINE_NUMS_STRIPPED',      ),
    (0x0008,      'IMAGE_FILE_LOCAL_SYMS_STRIPPED',     ),
    (0x0010,      'IMAGE_FILE_AGGRESSIVE_WS_TRIM',      ),
    (0x0020,      'IMAGE_FILE_LARGE_ADDRESS_AWARE',     ),
    (0x0080,      'IMAGE_FILE_BYTES_REVERSED_LO',       ),
    (0x0100,      'IMAGE_FILE_32BIT_MACHINE',           ),
    (0x0200,      'IMAGE_FILE_DEBUG_STRIPPED',          ),
    (0x0400,      'IMAGE_FILE_REMOVABLE_RUN_FROM_SWAP', ),
    (0x0800,      'IMAGE_FILE_NET_RUN_FROM_SWAP',       ),
    (0x1000,      'IMAGE_FILE_SYSTEM',                  ),
    (0x2000,      'IMAGE_FILE_DLL',                     ),
    (0x4000,      'IMAGE_FILE_UP_SYSTEM_ONLY',          ),
    (0x8000,      'IMAGE_FILE_BYTES_REVERSED_HI',       ),
  ]))]),
  ('array', 'sections', '1', 'NumberOfSections', ('struct', '', '!Section', [
    ('Name',                 ('scalar',  '<8s', secname)),
    ('VirtualSize',          ('scalar',  '<L',  '%d'   )),
    ('VirtualAddress',       ('scalar',  '<L',  '%d'   )),
    ('SizeOfRawData',        ('scalar',  '<L',  '%d'   )),
    ('PointerToRawData',     ('scalar',  '<L',  '0x%X' )),
    ('PointerToRelocations', ('scalar',  '<L',  '0x%X' )),
    ('PointerToLineNumbers', ('scalar',  '<L',  '0x%X' )),
    ('NumberOfRelocations',  ('scalar',  '<H',  '%d'   )),
    ('NumberOfLineNumbers',  ('scalar',  '<H',  '%d'   )),
    ('Charateristics',       ('flags',   '<L',  '0x%X', [
      (0x00000008, 'IMAGE_SCN_TYPE_NO_PAD'),
      (0x00000020, 'IMAGE_SCN_CNT_CODE'),
      (0x00000040, 'IMAGE_SCN_CNT_INITIALIZED_DATA'),
      (0x00000080, 'IMAGE_SCN_CNT_UNINITIALIZED_DATA'),
      (0x00000100, 'IMAGE_SCN_LNK_OTHER'),
      (0x00000200, 'IMAGE_SCN_LNK_INFO'),
      (0x00000800, 'IMAGE_SCN_LNK_REMOVE'),
      (0x00001000, 'IMAGE_SCN_LNK_COMDAT'),
      (0x00008000, 'IMAGE_SCN_GPREL'),
      (0x00020000, 'IMAGE_SCN_MEM_PURGEABLE'),
      (0x00020000, 'IMAGE_SCN_MEM_16BIT'),
      (0x00040000, 'IMAGE_SCN_MEM_LOCKED'),
      (0x00080000, 'IMAGE_SCN_MEM_PRELOAD'),
      (0x00F00000, 'IMAGE_SCN_ALIGN', {
        0x00100000: 'IMAGE_SCN_ALIGN_1BYTES',
        0x00200000: 'IMAGE_SCN_ALIGN_2BYTES',
        0x00300000: 'IMAGE_SCN_ALIGN_4BYTES',
        0x00400000: 'IMAGE_SCN_ALIGN_8BYTES',
        0x00500000: 'IMAGE_SCN_ALIGN_16BYTES',
        0x00600000: 'IMAGE_SCN_ALIGN_32BYTES',
        0x00700000: 'IMAGE_SCN_ALIGN_64BYTES',
        0x00800000: 'IMAGE_SCN_ALIGN_128BYTES',
        0x00900000: 'IMAGE_SCN_ALIGN_256BYTES',
        0x00A00000: 'IMAGE_SCN_ALIGN_512BYTES',
        0x00B00000: 'IMAGE_SCN_ALIGN_1024BYTES',
        0x00C00000: 'IMAGE_SCN_ALIGN_2048BYTES',
        0x00D00000: 'IMAGE_SCN_ALIGN_4096BYTES',
        0x00E00000: 'IMAGE_SCN_ALIGN_8192BYTES',
      }),
      (0x01000000, 'IMAGE_SCN_LNK_NRELOC_OVFL'),
      (0x02000000, 'IMAGE_SCN_MEM_DISCARDABLE'),
      (0x04000000, 'IMAGE_SCN_MEM_NOT_CACHED'),
      (0x08000000, 'IMAGE_SCN_MEM_NOT_PAGED'),
      (0x10000000, 'IMAGE_SCN_MEM_SHARED'),
      (0x20000000, 'IMAGE_SCN_MEM_EXECUTE'),
      (0x40000000, 'IMAGE_SCN_MEM_READ'),
      (0x80000000, 'IMAGE_SCN_MEM_WRITE'),
    ])),
    ('SectionData', ('ptr', 'PointerToRawData', ('blob', 'SizeOfRawData'))),
    ('Relocations', ('ptr', 'PointerToRelocations', ('array', '', '0', 'NumberOfRelocations', ('struct', '', '!Relocation', [
      ('VirtualAddress',   ('scalar', '<L', '0x%X')),
      ('SymbolTableIndex', ('scalar', '<L', '%d'  )),
      ('Type',             ('enum', '<H', '%d', ('MachineType', {
        0x14c: {
          0x0000: 'IMAGE_REL_I386_ABSOLUTE',
          0x0001: 'IMAGE_REL_I386_DIR16',
          0x0002: 'IMAGE_REL_I386_REL16',
          0x0006: 'IMAGE_REL_I386_DIR32',
          0x0007: 'IMAGE_REL_I386_DIR32NB',
          0x0009: 'IMAGE_REL_I386_SEG12',
          0x000A: 'IMAGE_REL_I386_SECTION',
          0x000B: 'IMAGE_REL_I386_SECREL',
          0x000C: 'IMAGE_REL_I386_TOKEN',
          0x000D: 'IMAGE_REL_I386_SECREL7',
          0x0014: 'IMAGE_REL_I386_REL32',
        },
        0x8664: {
          0x0000: 'IMAGE_REL_AMD64_ABSOLUTE',
          0x0001: 'IMAGE_REL_AMD64_ADDR64',
          0x0002: 'IMAGE_REL_AMD64_ADDR32',
          0x0003: 'IMAGE_REL_AMD64_ADDR32NB',
          0x0004: 'IMAGE_REL_AMD64_REL32',
          0x0005: 'IMAGE_REL_AMD64_REL32_1',
          0x0006: 'IMAGE_REL_AMD64_REL32_2',
          0x0007: 'IMAGE_REL_AMD64_REL32_3',
          0x0008: 'IMAGE_REL_AMD64_REL32_4',
          0x0009: 'IMAGE_REL_AMD64_REL32_5',
          0x000A: 'IMAGE_REL_AMD64_SECTION',
          0x000B: 'IMAGE_REL_AMD64_SECREL',
          0x000C: 'IMAGE_REL_AMD64_SECREL7',
          0x000D: 'IMAGE_REL_AMD64_TOKEN',
          0x000E: 'IMAGE_REL_AMD64_SREL32',
          0x000F: 'IMAGE_REL_AMD64_PAIR',
          0x0010: 'IMAGE_REL_AMD64_SSPAN32',
        },
      }))),
      ('SymbolName',       ('ptr', '+ PointerToSymbolTable * SymbolTableIndex 18', ('scalar',  '<8s', symname)))
    ])))),
  ])),
  ('ptr', 'PointerToSymbolTable', ('byte-array', 'symbols', '18', '* NumberOfSymbols 18',  ('struct', '', '!Symbol', [
    ('Name',                ('scalar',  '<8s', symname)),
    ('Value',               ('scalar',  '<L',  '%d'   )),
    ('SectionNumber',       ('scalar',  '<H',  '%d'   )),
    ('_Type',               ('scalar',  '<H',  None   )),
    ('SimpleType',          ('enum',    '& _Type 15',  '%d', {
      0: 'IMAGE_SYM_TYPE_NULL',
      1: 'IMAGE_SYM_TYPE_VOID',
      2: 'IMAGE_SYM_TYPE_CHAR',
      3: 'IMAGE_SYM_TYPE_SHORT',
      4: 'IMAGE_SYM_TYPE_INT',
      5: 'IMAGE_SYM_TYPE_LONG',
      6: 'IMAGE_SYM_TYPE_FLOAT',
      7: 'IMAGE_SYM_TYPE_DOUBLE',
      8: 'IMAGE_SYM_TYPE_STRUCT',
      9: 'IMAGE_SYM_TYPE_UNION',
      10: 'IMAGE_SYM_TYPE_ENUM',
      11: 'IMAGE_SYM_TYPE_MOE',
      12: 'IMAGE_SYM_TYPE_BYTE',
      13: 'IMAGE_SYM_TYPE_WORD',
      14: 'IMAGE_SYM_TYPE_UINT',
      15: 'IMAGE_SYM_TYPE_DWORD',
    })),                                # (Type & 0xF0) >> 4
    ('ComplexType',         ('enum',    '>> & _Type 240 4',  '%d', {
      0: 'IMAGE_SYM_DTYPE_NULL',
      1: 'IMAGE_SYM_DTYPE_POINTER',
      2: 'IMAGE_SYM_DTYPE_FUNCTION',
      3: 'IMAGE_SYM_DTYPE_ARRAY',
    })),
    ('StorageClass',        ('enum',    '<B',  '%d', {
      -1:  'IMAGE_SYM_CLASS_END_OF_FUNCTION',
      0: 'IMAGE_SYM_CLASS_NULL',
      1: 'IMAGE_SYM_CLASS_AUTOMATIC',
      2: 'IMAGE_SYM_CLASS_EXTERNAL',
      3: 'IMAGE_SYM_CLASS_STATIC',
      4: 'IMAGE_SYM_CLASS_REGISTER',
      5: 'IMAGE_SYM_CLASS_EXTERNAL_DEF',
      6: 'IMAGE_SYM_CLASS_LABEL',
      7: 'IMAGE_SYM_CLASS_UNDEFINED_LABEL',
      8: 'IMAGE_SYM_CLASS_MEMBER_OF_STRUCT',
      9: 'IMAGE_SYM_CLASS_ARGUMENT',
      10: 'IMAGE_SYM_CLASS_STRUCT_TAG',
      11: 'IMAGE_SYM_CLASS_MEMBER_OF_UNION',
      12: 'IMAGE_SYM_CLASS_UNION_TAG',
      13: 'IMAGE_SYM_CLASS_TYPE_DEFINITION',
      14: 'IMAGE_SYM_CLASS_UNDEFINED_STATIC',
      15: 'IMAGE_SYM_CLASS_ENUM_TAG',
      16: 'IMAGE_SYM_CLASS_MEMBER_OF_ENUM',
      17: 'IMAGE_SYM_CLASS_REGISTER_PARAM',
      18: 'IMAGE_SYM_CLASS_BIT_FIELD',
      100: 'IMAGE_SYM_CLASS_BLOCK',
      101: 'IMAGE_SYM_CLASS_FUNCTION',
      102: 'IMAGE_SYM_CLASS_END_OF_STRUCT',
      103: 'IMAGE_SYM_CLASS_FILE',
      104: 'IMAGE_SYM_CLASS_SECTION',
      105: 'IMAGE_SYM_CLASS_WEAK_EXTERNAL',
      107: 'IMAGE_SYM_CLASS_CLR_TOKEN',
    })),
    ('NumberOfAuxSymbols',  ('scalar',  '<B',  '%d'  )),
    ('AuxillaryData', ('blob', '* NumberOfAuxSymbols 18')),
  ])))]

#
# Definition Interpreter
#

import sys, types, struct, re

Input = None
Stack = []
Fields = {}

Indent = 0
NewLine = True

def indent():
  global Indent
  Indent += 1

def dedent():
  global Indent
  Indent -= 1

def write(input):
  global NewLine
  output = ""

  for char in input:

    if NewLine:
      output += Indent * '  '
      NewLine = False

    if char == chr(0):
      char = ' '

    output += char

    if char == '\n':
      NewLine = True

  sys.stdout.write(output)

def read(format):
  return struct.unpack(format, Input.read(struct.calcsize(format)))

def read_cstr():
  output = ""
  while True:
    char = Input.read(1)
    if len(char) == 0:
      raise RuntimeError ("EOF while reading cstr")
    if char == '\0':
      break
    output += char
  return output

def push_pos(seek_to = None):
  Stack [0:0] = [Input.tell()]
  if seek_to:
    Input.seek(seek_to)

def pop_pos():
  assert(len(Stack) > 0)
  Input.seek(Stack[0])
  del Stack[0]

def print_binary_data(size):
  value = ""
  bytes = ""
  text = ""
  while size > 0:
    if size >= 16:
      data = Input.read(16)
      size -= 16
    else:
      data = Input.read(size)
      size = 0
    value += data
    for index in xrange(16):
      if index < len(data):
        ch = ord(data[index])
        bytes += "\\x%02X" % ch
        if ch >= 0x20 and ch <= 0x7F:
          text += data[index]
        else:
          text += "."

  write("\"%s\" # |%s|\n" % (bytes, text))
  return value

idlit = re.compile("[a-zA-Z_][a-zA-Z0-9_-]*")
numlit = re.compile("[0-9]+")

def read_value(expr):

  input = iter(expr.split())

  def eval():

    token = input.next()

    if expr == 'cstr':
      return read_cstr()
    if expr == 'true':
      return True
    if expr == 'false':
      return False

    if token == '+':
      return eval() + eval()
    if token == '-':
      return eval() - eval()
    if token == '*':
      return eval() * eval()
    if token == '/':
      return eval() / eval()
    if token == '&':
      return eval() & eval()
    if token == '|':
      return eval() | eval()
    if token == '>>':
      return eval() >> eval()
    if token == '<<':
      return eval() << eval()

    if len(token) > 1 and token[0] in ('=', '@', '<', '!', '>'):
      val = read(expr)
      assert(len(val) == 1)
      return val[0]

    if idlit.match(token):
      return Fields[token]
    if numlit.match(token):
      return int(token)

    raise RuntimeError("unexpected token %s" % repr(token))

  value = eval()

  try:
    input.next()
  except StopIteration:
    return value
  raise RuntimeError("unexpected input at end of expression")

def write_value(format,value):
  format_type = type(format)
  if format_type is types.StringType:
    write(format % value)
  elif format_type is types.FunctionType:
    write_value(format(value), value)
  elif format_type is types.TupleType:
    Fields['this'] = value
    handle_element(format)
  elif format_type is types.NoneType:
    pass
  else:
    raise RuntimeError("unexpected type: %s" % repr(format_type))

def handle_scalar(entry):
  iformat = entry[1]
  oformat = entry[2]

  value = read_value(iformat)

  write_value(oformat, value)

  return value

def handle_enum(entry):
  iformat = entry[1]
  oformat = entry[2]
  definitions = entry[3]

  value = read_value(iformat)

  if type(definitions) is types.TupleType:
    selector = read_value(definitions[0])
    definitions = definitions[1][selector]

  if value in definitions:
    description = definitions[value]
  else:
    description = "unknown"

  write("%s # (" % description)
  write_value(oformat, value)
  write(")")

  return value

def handle_flags(entry):
  iformat = entry[1]
  oformat = entry[2]
  definitions = entry[3]

  value = read_value(iformat)

  write('[')
  for entry in definitions:
    mask = entry[0]
    name = entry[1]
    if len (entry) == 3:
      map = entry[2]
      selection = value & mask
      if selection in map:
        write("%s, " % map[selection])
      else:
        write("%s <%d>, " % (name, selection))
    elif len(entry) == 2:
      if value & mask != 0:
        write("%s, " % name)
  write('] # ')
  write_value(oformat, value)

  return value

def handle_struct(entry):
  global Fields
  members = entry[3]

  newFields = {}

  name = ''
  t    = ''
  if len(entry[1]) > 0:
    name = "%s: " % entry[1]
  if len(entry[2]) > 0:
    t = entry[2]
  write("%s%s\n" % (name, t))
  indent()

  for member in members:
    name = member[0]
    type = member[1]

    if name[0] != "_":
      write("%s: " % name)

    value = handle_element(type)

    if name[0] != "_":
      write("\n")

    Fields[name] = value
    newFields[name] = value

  dedent()
  # write("}")

  return newFields

def handle_array(entry):
  name = entry[1]
  start_index = entry[2]
  length = entry[3]
  element = entry[4]

  newItems = []

  if len(name) > 0:
    write("%s:\n" % name)
  else:
    write('\n')
  indent()

  start_index = read_value(start_index)
  value = read_value(length)

  for index in xrange(value):
    write('- ')
    value = handle_element(element)
    write("\n")
    newItems.append(value)

  dedent()

  return newItems

def handle_byte_array(entry):
  name = entry[1]
  ent_size = entry[2]
  length = entry[3]
  element = entry[4]

  newItems = []

  if len(name) > 0:
    write("%s:\n" % name)
  else:
    write('\n')
  indent()

  item_size = read_value(ent_size)
  value = read_value(length)
  end_of_array = Input.tell() + value

  prev_loc = Input.tell()
  index = 0
  while Input.tell() < end_of_array:
    write('- ')
    value = handle_element(element)
    write("\n")
    newItems.append(value)
    index += (Input.tell() - prev_loc) / item_size
    prev_loc = Input.tell()

  dedent()

  return newItems

def handle_ptr(entry):
  offset = entry[1]
  element = entry[2]

  value = None
  offset = read_value(offset)

  if offset != 0:

    push_pos(offset)

    value = handle_element(element)

    pop_pos()

  else:
    write("None")

  return value

def handle_blob(entry):
  length = entry[1]

  write("\n")
  indent()

  value = print_binary_data(read_value(length))

  dedent()

  return value

def handle_element(entry):
  handlers = {
    'struct':      handle_struct,
    'scalar':      handle_scalar,
    'enum':        handle_enum,
    'flags':       handle_flags,
    'ptr':         handle_ptr,
    'blob':        handle_blob,
    'array':       handle_array,
    'byte-array':  handle_byte_array,
  }

  if not entry[0] in handlers:
    raise RuntimeError ("unexpected type '%s'" % str (entry[0]))

  return handlers[entry[0]](entry)

def dump_coff(stream):
  global Input
  Input = stream
  for e in file:
    handle_element(e)

import yaml, types, struct

def get(d, key, default):
  v = d.get(key, default)
  if v is None:
    v = default
  return v

def default(v, d):
  if v is None:
    return d
  return v

SIZEOF_HEADER     = 20
SIZEOF_SECTION    = 40
SIZEOF_RELOCATION = 10
SIZEOF_SYMBOL     = 18

MachineTypes = {
    'IMAGE_FILE_MACHINE_I386': 0x14C
  }

HeaderChars = {
  }

SecChars = {
    'IMAGE_SCN_CNT_CODE':      0x00000020,
    'IMAGE_SCN_ALIGN_16BYTES': 0x00500000,
    'IMAGE_SCN_MEM_EXECUTE':   0x20000000,
    'IMAGE_SCN_MEM_READ':      0x40000000
  }

SimpleType = {
    'IMAGE_SYM_TYPE_NULL': 0
  }

ComplexType = {
    'IMAGE_SYM_DTYPE_NULL':     0,
    'IMAGE_SYM_DTYPE_FUNCTION': 2
  }

StorageClass = {
    'IMAGE_SYM_CLASS_EXTERNAL': 2,
    'IMAGE_SYM_CLASS_STATIC':   3
  }

RelocationTypes = {
    'IMAGE_REL_I386_DIR32': 6
  }

class COFFHeader(yaml.YAMLObject):
  yaml_tag = u'!Header'
  MachineType          = None
  NumberOfSections     = None
  TimeDateStamp        = None
  PointerToSymbolTable = None
  NumberOfSymbols      = None
  SizeOfOptionalHeader = None
  Characteristics      = None

  def layout(self, coff):
    self.MachineType = default(self.MachineType, 0)
    self.NumberOfSections = default(self.NumberOfSections, len(coff.sections))
    self.TimeDateStamp = default(self.TimeDateStamp, 0)
    self.SizeOfOptionalHeader = default(self.SizeOfOptionalHeader, 0)
    self.PointerToSymbolTable = default(self.PointerToSymbolTable,
            (SIZEOF_HEADER + self.SizeOfOptionalHeader) + \
            (self.NumberOfSections * SIZEOF_SECTION))
    self.NumberOfSymbols = default(self.NumberOfSymbols, len(coff.symbols))
    self.Characteristics = default(self.Characteristics, 0)

    if type(self.MachineType) == types.StringType:
      self.MachineType = MachineTypes[self.MachineType]

    if type(self.Characteristics) == types.ListType:
      res = 0
      for c in self.Characteristics:
        res |= HeaderChars[c]
      self.Characteristics = res

  def write(self):
    return [(struct.pack('<H', self.MachineType), 'MachineType'),
            (struct.pack('<H', self.NumberOfSections), 'NumberOfSections'),
            (struct.pack('<I', self.TimeDateStamp), 'TimeDateStamp'),
            (struct.pack('<I', self.PointerToSymbolTable),
              'PointerToSymbolTable'),
            (struct.pack('<I', self.NumberOfSymbols), 'NumberOfSymbols'),
            (struct.pack('<H', self.SizeOfOptionalHeader),
              'SizeOfOptionalHeader'),
            (struct.pack('<H', self.Characteristics), 'Characteristics')]

class COFFSection(yaml.YAMLObject):
  yaml_tag = u'!Section'
  Index                = None

  Name                 = None
  VirtualSize          = None
  VirtualAddress       = None
  SizeOfRawData        = None
  PointerToRawData     = None
  PointerToRelocations = None
  PointerToLineNumbers = None
  NumberOfRelocations  = None
  NumberOfLineNumbers  = None
  Characteristics      = None
  SectionData          = None
  Relocations          = []

  def layout(self, coff, index):
    self.Index = default(self.Index, index)
    if type(self.Name) == types.StringType:
      if len(self.Name) > 8:
        self.Name = '/%d' % coff.strtab.add(self.Name)
    else:
      self.Name = '/%d' % self.Name
    self.VirtualSize = default(self.VirtualSize, 0)
    self.VirtualAddress = default(self.VirtualAddress, 0)
    self.SizeOfRawData = default(self.SizeOfRawData, len(self.SectionData))
    self.PointerToRawData = default(self.PointerToRawData, 0)
    self.PointerToRelocations = default(self.PointerToRelocations, 0)
    self.PointerToLineNumbers = default(self.PointerToLineNumbers, 0)
    self.NumberOfRelocations = default(self.NumberOfRelocations, 0)
    self.NumberOfLineNumbers = default(self.NumberOfLineNumbers, 0)
    self.Characteristics = default(self.Characteristics, 0)

    if type(self.Characteristics) == types.ListType:
      res = 0
      for c in self.Characteristics:
        res |= SecChars[c]
      self.Characteristics = res

    for reloc in self.Relocations:
      reloc.layout(coff)

  def decode_name(self, coff, name):
    if name[0] == '/':
      return coff.strtab.get(int(name[1:]))
    else:
      return name

  def write_header(self, coff):
    return [(None, 'Section %d' % self.Index),
            (struct.pack('8s', self.Name),
              'Name: %s' % self.decode_name(coff, self.Name)),
            (struct.pack('<I', self.VirtualSize), 'VirtualSize'),
            (struct.pack('<I', self.VirtualAddress), 'VirtualAddress'),
            (struct.pack('<I', self.SizeOfRawData), 'SizeOfRawData'),
            (struct.pack('<I', self.PointerToRawData), 'PointerToRawData'),
            (struct.pack('<I', self.PointerToRelocations),
              'PointerToRelocations'),
            (struct.pack('<I', self.PointerToLineNumbers),
              'PointerToLineNumbers'),
            (struct.pack('<H', self.NumberOfRelocations),
              'NumberOfRelocations'),
            (struct.pack('<H', self.NumberOfLineNumbers),
              'NumberOfLineNumbers'),
            (struct.pack('<I', self.Characteristics), 'Characteristics')]

  def write_contents(self):
    return [(None, 'Section %d Data' % self.Index),
            (self.SectionData, 'Data')]

  def write_relocations(self):
    ret = []
    for r in self.Relocations:
      ret.append([(struct.pack('<I', r.VirtualAddress), 'VirtualAddress'),
                  (struct.pack('<I', r.SymbolTableIndex),'SymbolTableIndex'),
                  (struct.pack('<H', r.Type), 'Type')])
    return ret

class COFFRelocation(yaml.YAMLObject):
  yaml_tag = u'!Relocation'
  VirtualAddress   = None
  SymbolTableIndex = None
  Type             = None

  def layout(self, coff):
    if type(self.Type) == types.StringType:
      self.Type = RelocationTypes[self.Type]

class COFFSymbol(yaml.YAMLObject):
  yaml_tag = u'!Symbol'
  Index              = None

  Name               = None
  Value              = None
  SectionNumber      = None
  SimpleType         = None
  ComplexType        = None
  StorageClass       = None
  NumberOfAuxSymbols = None
  AuxillaryData      = None

  def layout(self, coff):
    if type(self.Name) == types.StringType:
      if len(self.Name) > 8:
        self.Name = struct.pack('<II', 0, coff.strtab.add(self.Name))
    else:
      self.Name = struct.pack('<II', 0, self.Name)
    self.Value = default(self.Value, 0)
    self.SectionNumber = default(self.SectionNumber, 0)
    self.SimpleType = default(self.SimpleType, 0)
    self.ComplexType = default(self.ComplexType, 0)
    self.StorageClass = default(self.StorageClass, 0)
    self.NumberOfAuxSymbols = default(self.NumberOfAuxSymbols, 0)
    self.AuxillaryData = default(self.AuxillaryData, "")

    if type(self.SimpleType) == types.StringType:
      self.SimpleType = SimpleType[self.SimpleType]
    if type(self.ComplexType) == types.StringType:
      self.ComplexType = ComplexType[self.ComplexType]
    if type(self.StorageClass) == types.StringType:
      self.StorageClass = StorageClass[self.StorageClass]

  def decode_name(self, coff, name):
    if name[0] != 0:
      return name
    else:
      bytes = coff.strtab.get(struct.unpack('<II', name)[1])

  def write(self, coff):
    return [(None, 'Symbol %d' % self.Index),
            (struct.pack('8s', self.Name),
              'Name: %s' % self.decode_name(coff, self.Name)),
            (struct.pack('<I', self.Value), 'Value'),
            (struct.pack('<H', self.SectionNumber), 'SectionNumber'),
            (struct.pack('<H', self.SimpleType | (self.ComplexType << 4)),
              'Type'),
            (struct.pack('<B', self.StorageClass), 'StorageClass'),
            (struct.pack('<B', self.NumberOfAuxSymbols),
              'NumberOfAuxSymbols'),
            (self.AuxillaryData, 'Auxillary Data')]

class COFFStringTable:
  def __init__(self, strings):
    self.offset = 4 # Reserve space for size info
    self.htab   = {}
    if type(strings) == types.StringType:
      self.add(strings)
    elif type(strings) == types.ListType:
      for s in strings:
        self.add(s)

  def add(self, string):
    try:
      return self.lookup(string)
    except KeyError:
      self.htab[string] = offset = self.offset
      self.offset += len(string) + 1
      return offset

  def lookup(self, string):
    return self.htab[string]

  def get(self, index):
    for k, v in self.htab.iteritems():
      if v == index:
        return k
    raise ValueError

  def write(self):
    ret = [(None, 'String Table'),
            (struct.pack('<I', self.offset), 'Size')]
    l = self.htab.items()
    l.sort(lambda x, y: cmp(x[1], y[1]))
    for (ss, oo) in l:
      ret.append((ss + '\0', ss))
    return ret

def write_hexbytes_comment(data):
  for p in data:
    if p[0] is not None:
      for c in p[0]:
        print '%02X' % ord(c),
    if p[1] is not None:
      print('# ' + p[1])

class COFF:
  def __init__(self, yamldef):
    self.yd = yamldef
    self.header = get(self.yd, 'header', COFFHeader())
    self.sections = get(self.yd, 'sections', [])
    self.symbols = get(self.yd, 'symbols', [])
    self.strtab = COFFStringTable(get(self.yd, 'strtab', []))

  def layout(self):
    self.header.layout(self)

    curoffset = self.header.PointerToSymbolTable + \
                  self.header.NumberOfSymbols * SIZEOF_SYMBOL
    for i, s in enumerate(self.sections):
      s.layout(self, i)
      if s.PointerToRawData == 0 and s.SizeOfRawData != 0:
        s.PointerToRawData = curoffset
        curoffset += s.SizeOfRawData
        s.PointerToRelocations = curoffset
        curoffset += s.NumberOfRelocations * SIZEOF_RELOCATION

    index = 0
    for s in self.symbols:
      s.Index = index
      s.layout(self)
      index += 1 + s.NumberOfAuxSymbols

  def write(self):
    # Write out header.
    write_hexbytes_comment(self.header.write())
    # Write out section headers.
    for sec in self.sections:
      write_hexbytes_comment(sec.write_header(self))
    # Write out section data.
    for sec in self.sections:
      write_hexbytes_comment(sec.write_contents())
      write_hexbytes_comment(sec.write_relocations())
    # Write out symbol table.
    for symb in self.symbols:
      write_hexbytes_comment(symb.write(self))
    # Write out strtab
    write_hexbytes_comment(self.strtab.write())

def make_coff(stream):
  yd = yaml.load(stream)
  c = COFF(yd)
  c.layout()
  return c
