import argparse
parser = argparse.ArgumentParser()
parser.add_argument("--file")
args = parser.parse_args()

import re
labels_with_no_type = []

file_path = "decompiler_out/jak1/{}_disasm.gc".format(args.file)
with open(file_path) as f:
  # Find all
  content = f.readlines()
  for line in content:
    labels_with_no_type = labels_with_no_type + re.findall(r'L\d+', line)

# dedup list
labels_with_no_type = list(dict.fromkeys(labels_with_no_type))

# let's go try to identify the types from the IR2 file if we can
label_lines = []
file_path = "decompiler_out/jak1/{}_ir2.asm".format(args.file)
with open(file_path) as f:
  # Find all
  content = f.readlines()
  prev_line = ""
  for i, line in enumerate(content):
    if line.startswith("L"):
      for label in labels_with_no_type:
        if line.startswith("{}:".format(label)):
          # Check if the previous line has a `.type`
          prev_line = content[i-1]
          if ".type" in prev_line:
            the_type = prev_line.split(".type ")[1].strip()
            label_lines.append("[\"{}\", \"{}\", true]".format(label, the_type))
            labels_with_no_type.remove(label)
            break
          # Otherwise, lets see if it's a lambda, check the last 10 lines (hopefully this is enough)
          found_it = False
          for j in range(20):
            temp_line = content[i-j]
            if "anon-function" in temp_line:
              label_lines.append("[\"{}\", \"_lambda_\", true]".format(label))
              labels_with_no_type.remove(label)
              found_it = True
              break
          if found_it:
            break

# Print out the labels
print("Here are the labels I couldn't find a type for:")
for label in labels_with_no_type:
  print("- {}".format(label))
print("And here are the ones I could:")
print(",\n".join(label_lines))
