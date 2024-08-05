import json
import os
import re
import sys
import hashlib # Hashing shader source files

from pathlib import Path

# GLOBALS
G_VULKAN_SDK    = os.environ[ "VULKAN_SDK" ]
G_SHADER_TYPES  = [ "vert", "geom", "frag", "comp" ]
G_METADATA_EXT  = "meta"

G_PROJECT_DIR       = None
G_SOURCE_DIR        = None
G_SOURCE_PATH       = None
G_INCLUDE_DIR       = None
G_INCLUDE_PATH      = None
G_OUTPUT_DIR        = None
G_OUTPUT_PATH       = None
G_SHADER_COMPILER   = None


# Log a common message
def LogCommon(msg: str):
    print(f'[ SHADERS ] {msg}')


# Log a shader build error
def LogError(msg: str):
    LogCommon(f'[ ERROR ] {msg}')
    

# Log a shader build warning
def LogWarning(msg: str):
    LogCommon(f'[ WARNING ] {msg}')
    

# Log a generic info message
def LogInfo(msg: str):
    LogCommon(f'[ INFO ] {msg}')
    

# Log a build step
def LogBuildStep(msg: str):
    LogCommon(f'[ {msg.upper()} ]')
    

# Takes in the checksum source in binary format. Usually this comes from calling read() on a
# file handle marked with the 'b' (binary) flag. The resulting SHA-256 hex is truncated because
# we really don't need a huge checksum for our purposes (not trying to use cryptographically secure
# hashes or anything or sorts). I could research more about hashes and use one that returns a smaller
# has but...meh
def ShaderChecksum(src):
    shaObj = hashlib.sha256()
    shaObj.update(src)
    hex = shaObj.hexdigest()
    return hex[:16]
    

# Returns a tuple containing the full output path, and the name of the shader in that respective order
def GetShaderOutputPathFromSourcePath(shaderPath: str):

        shaderName = (shaderPath.suffix)[1:]
        parentDirName = shaderPath.parent.name
        return ( os.path.normpath(f"{G_OUTPUT_PATH}/{parentDirName}"), shaderName )
        
        
# Converts the given path from an absolute path to one that's relative to the project directory.
# Returns a normalized relative path
def ConvertToRelativePath(path: str) -> str:
    return os.path.normpath(f"{path.replace(G_PROJECT_DIR, '')}")
        

def ParseLayoutUniformStruct(layoutStructure: str):

    # There is no null check because it's done by the caller. It's actually valid for this to be missing from the layout
        
    structureRegexStr = r"\{(.*?)\}\s*(.*?)$"
    structureMatch = re.match(structureRegexStr, layoutStructure, re.DOTALL) 
    layoutStructureDict = {}
    
    if structureMatch is not None:
        layoutStructureDict["uniform_name"] = structureMatch.group(2)
        layoutStructureDict["data_members"] = []
        
        dataGroup = structureMatch.group(1).split('\n')
        dataGroupRegexStr = r"\s*(.*?)\s+(.*?);\s*(.*?)?$"
        for data in dataGroup:
            dataMatch = re.match(dataGroupRegexStr, data)
            if dataMatch is not None:
                dataComment = dataMatch.group(3) if dataMatch.group(3) is not None else ""
                layoutStructureDict["data_members"].append({ "type" : dataMatch.group(1), "name" : dataMatch.group(2), "comment" : dataComment })
    
    return layoutStructureDict


def ParseLayoutDeclaration(layoutDeclaration: str or None):

    if layoutDeclaration is None:
        LogError("Failed to parse layout declaration! Layout declaration is None")
        return {}
        
    # Format the layout declaration by removing trailing and leading whitespace, and then splitting off remaining whitespace
    layoutDeclaration = layoutDeclaration.strip().split(' ')
    layoutDeclarationLen = len(layoutDeclaration)
    
    layoutDeclarationDict = {}
    layoutDeclarationDict["attributes"] = layoutDeclaration[:layoutDeclarationLen]
    
    return layoutDeclarationDict


def ParseLayoutQualifiers(layoutQualifiers: str or None):

    if layoutQualifiers is None:
        LogError("Failed to parse layout qualifiers! Layout qualifiers is None")
        return {}

    # Format the layout qualifiers by removing whitespace, then splitting them off of the commas
    layoutQualifiersStr = ''.join(layoutQualifiers.split())
    layoutQualifierList = layoutQualifiersStr.split(',')
    
    # Split the list once again based off equals sign. In the end, we'll have dictionary
    layoutQualifierDict = { }
    for layoutQualifier in layoutQualifierList:
        qualifierPair = layoutQualifier.split('=')
        qualifierPairLen = len(qualifierPair)
        if qualifierPairLen == 1:
            layoutQualifierDict[qualifierPair[0]] = None
        elif qualifierPairLen == 2: # We found a good split!
            # Consider if the string represents an integer. If not, leave it as a string
            # TODO - Consider floats. I can't think of a case where floats would be valid, but it might come up later!
            if qualifierPair[1].isdigit():
                layoutQualifierDict[qualifierPair[0]] = int(qualifierPair[1])
            else:
                layoutQualifierDict[qualifierPair[0]] = qualifierPair[1]
        else:
            LogError(f'Found a malformed qualifier pair "{qualifierPair}" in layout "{layoutMatch.group(0)}"')
    
    return layoutQualifierDict


def GenerateShaderMetadata(shaderPath: str, fullOutputPath: str, shaderName: str):
    
    #
    # Detects anything matching a layout declaration, whether multi-line of single-line.
    # The group matches are as follows for this declaration example:
    #
    # layout(set = 0, binding = 0) uniform ViewProjObject {
    #      mat4 view;
    #      mat4 proj;
    # } viewProjUBO;
    #
    # GROUP 1 - Matches the layout "qualifiers", or anything in parenthesis after the literal "layout". In this case:
    #  "set = 0, binding = 0"
    #
    # GROUP 2 - Matches anything after the "qualifiers" and before either a sub-declaration of the end of the declaration (denoted with a ';'). In this case:
    #  " uniform ViewProjObject "
    #
    # GROUP 3 (OPTIONAL) - Matches any uniform struct, if applicable. Since the example refers to a uniform struct, it would match as follows. In the case where the match isn't about a uniform struct, this group would be empty
    # "{
    #      mat4 view;
    #      mat4 proj;
    #  } viewProjUBO"
    #
    layoutStringRegex = r"layout\s*\((.*?)\)(.*?)(\{.+?\}.*?)?;"
        
    # Create new metadata file or overwrite existing one
    metadataFilePath = f'{fullOutputPath}/{shaderName}.{G_METADATA_EXT}'
    metadataFileHandle = open(metadataFilePath, "w")
    if metadataFileHandle is None:
        LogError(f'Failed to find or create metadata file: "{metadataFilePath}"')
        return
        
    layoutJSON = []
    
    with open(shaderPath, "r") as shaderSource:
    
        for match in re.finditer(layoutStringRegex, shaderSource.read(), re.DOTALL):
            
            # Convert match object to string
            layoutStr = match.group(0)
            
            # Parse the layout qualifier(s) within parenthesis (e.g. "location = 0", "set = 0", "rgba32f", etc)
            layoutQualifiers = ParseLayoutQualifiers(match.group(1))
            
            # Parse the layout declaration (e.g. "uniform readonly image2D" for the type and "inTexture" as the name)
            layoutDeclaration = ParseLayoutDeclaration(match.group(2))

            # Parse the uniform struct, if applicable (e.g. "{ mat4 view; mat4 proj; } viewProjUBO" for a uniform declaration)
            layoutUniformStruct = match.group(3)
            if layoutUniformStruct is not None:
                layoutUniformStruct = ParseLayoutUniformStruct(layoutUniformStruct) # Re-assign from string to dictionary
                
            
            # Now build the final JSON
            # NOTE - If a uniform struct exists, we require the layout's name to be AFTER the uniform struct
            layoutName = None
            if layoutUniformStruct is not None:
                layoutName = layoutUniformStruct["uniform_name"]
            else:
                layoutName = layoutDeclaration["attributes"][-1] # The name should come last in the list in this case
                
            layoutDeclarationSize = len(layoutDeclaration["attributes"])
            layoutJSON.append({ "name" : layoutName, "qualifiers" : layoutQualifiers, "attributes" : layoutDeclaration["attributes"][:layoutDeclarationSize - 1] })
                    
        
        # Finally, write out the JSON
        metadataFileHandle.write(json.dumps(layoutJSON, indent = 2))
        metadataFileHandle.close()
        

def CompileShaderByteCode(shaderPath: str, fullOutputPath: str, shaderName: str):
    
    shaderOutputPath = f"{fullOutputPath}/{shaderName}.spv"
        
    # Compile the input shader
    os.system(f'{G_SHADER_COMPILER} "{shaderPath}" -O -I "{G_INCLUDE_PATH}" -o "{shaderOutputPath}"') # Preprocesses, compiles and links input shader (produces optimized binary SPV)
    #os.system(f'{G_SHADER_COMPILER} "{shaderPath}" -O0 -I "{G_INCLUDE_PATH}" -o "{fullOutputPath}/{shaderName}.spv"') # Preprocesses, compiles and links input shader (produces un-optimized binary SPV)
    #os.system(f'{G_SHADER_COMPILER} "{shaderPath}" -E -I "{G_INCLUDE_PATH}" -o "{fullOutputPath}/{shaderName}.pp"') # Preprocesses input shader
        
        
def ComputeDependencies(shaderPath: str, computedDependencies: list):

    includeFileRegex = r"#include\s+\"(.*?)\""
    with open(shaderPath, "r") as f:
    
        fileContents = f.read()
        matches = re.finditer(includeFileRegex, fileContents)
        
        if matches is None:
            return
            
        for match in matches:
        
            # Append full patch for all matches
            dependencyPath = os.path.normpath(f'{G_INCLUDE_PATH}/{match.group(1)}')
            
            # Only append dependencies we haven't already run into
            if dependencyPath not in computedDependencies:
                computedDependencies.append(dependencyPath)
                    
                # Recursively find all other dependencies
                ComputeDependencies(dependencyPath, computedDependencies)
        
        
def CheckShaderDependencies(shaderPath: str, checksums: dict, dependencyComputedHashes: dict) -> list:

    # Recursively compute all the dependencies for the current shader
    computedDependencies = []
    ComputeDependencies(shaderPath, computedDependencies)
    
    # Find hashes for all dependencies we haven't hashed yet
    dependenciesRequiringRecompilation = []
    for dependency in computedDependencies:
    
        # If we haven't hashed this dependency, hash it now
        if dependency not in dependencyComputedHashes:
            with open(dependency, "rb") as f:
                dependencyComputedHashes[dependency] = ShaderChecksum(f.read())
                
        # Now we have a hash to compare against
        newDependencyHash = dependencyComputedHashes[dependency]
        shortDependencyPath = dependency.replace(G_PROJECT_DIR, "")
        
        if shortDependencyPath not in checksums:
            # Failed to find a checksum for the dependency in checksum.json, so we must compile dependency + source shader anyway
            # The hashes for the dependencies are all appended after all shaders are built so we can properly re-build all shaders
            # that had to indirectly be re-built because of the same dependency
            dependenciesRequiringRecompilation.append(shortDependencyPath)
            continue
            
        existingDependencyHash = checksums[shortDependencyPath]
        if existingDependencyHash != newDependencyHash:
            # We must re-compile the dependency and - indirectly - the source shader
            dependenciesRequiringRecompilation.append(shortDependencyPath)
            
    return dependenciesRequiringRecompilation


def main():

    global G_PROJECT_DIR
    global G_SOURCE_DIR
    global G_SOURCE_PATH
    global G_INCLUDE_DIR
    global G_INCLUDE_PATH
    global G_OUTPUT_DIR
    global G_OUTPUT_PATH
    global G_SHADER_COMPILER
    
    LogBuildStep("BUILDING SHADERS START")

    G_PROJECT_DIR = str(Path(sys.argv[0]).parent.absolute().parent.parent) # This is ugly, but for some reason it won't let me do .parent 3 times...
    
    # Shader source directory
    G_SOURCE_DIR = sys.argv[1]
    G_SOURCE_PATH = f'{G_PROJECT_DIR}/{G_SOURCE_DIR}'
    if G_SOURCE_DIR is None or not os.path.exists(G_SOURCE_PATH):
        LogError(f'Please provide a valid shader source directory, the following path does not exist: {G_SOURCE_PATH}')
        return
    
    # Shader output directory
    G_OUTPUT_DIR = sys.argv[2]
    G_OUTPUT_PATH = f'{G_PROJECT_DIR}/{G_OUTPUT_DIR}'
    if G_OUTPUT_DIR is None:
        LogError(f'Please provide a valid shader output directory')
        return
    
    # Check that the output directory exists. If not, create it
    os.makedirs(G_OUTPUT_PATH, exist_ok=True)
    
    # Shader include directory
    G_INCLUDE_DIR = sys.argv[3]
    G_INCLUDE_PATH = os.path.normpath(f'{G_PROJECT_DIR}/{G_INCLUDE_DIR}')
    if G_INCLUDE_DIR is None or not os.path.exists(G_INCLUDE_PATH):
        LogError(f'Please provide a valid shader include directory, the following path does not exist: {G_INCLUDE_PATH}')
        return
    
    # Ensure the Vulkan SDK is installed
    if G_VULKAN_SDK is None:
        LogError("Please install the Vulkan SDK to compile shaders")
        return
        
    G_SHADER_COMPILER = os.path.normpath(f"{G_VULKAN_SDK}/Bin/glslc.exe")
    
    # Get list of source shaders to compile
    shaderList = []
    for shaderType in G_SHADER_TYPES:
        for path in Path(f"{G_PROJECT_DIR}/{G_SOURCE_DIR}").rglob(f"*.{shaderType}"):
            shaderList.append(path)
    
    # Attempt to read in shader checksums from previous compilations
    checksumFileName = "checksum.json"
    checksumFilePath = f"{G_OUTPUT_PATH}/{checksumFileName}"
    
    LogBuildStep("LOADING CHECKSUMS START")
    
    checksums = {}
    if os.path.isfile(checksumFilePath):
        checksumFileHandle = open(checksumFilePath, "r+")
        
        # Read in the checksums
        try:
            if checksumFileHandle is not None:
                checksumContents = checksumFileHandle.read()
                checksumFileHandle.seek(0)
                checksums = json.loads(checksumContents)
                LogInfo(f'Checksum file loaded "{checksumFileName}"')
        except Exception as e:
            LogError('Failed to load shader checksum file contents! Exception raised: {e}')
            return
        
    else: # file doesn't exists so we're going to create it beforehand
        checksumFileHandle = open(checksumFilePath, "w")
        
    LogBuildStep("LOADING CHECKSUMS END")
    LogBuildStep("SHADER COMPILATION/METADATA START")
    
    # Compile all shaders that require compilation
    newHashesFound = False
    dependencyComputedHashes = {} # Stores the hashes for all the dependencies (a.k.a included .glsl files). We cache them to avoid re-calculating hashes for dependencies
    LogInfo(f'Using shader compiler "{G_SHADER_COMPILER}"')
    for shaderPath in shaderList:
    
        shaderPathStr = str(shaderPath)
        outputPath = GetShaderOutputPathFromSourcePath(shaderPath)
        fullOutputPath = outputPath[0]
        shaderName = outputPath[1]
        
        fullShaderDstPath = os.path.normpath(str(f'{fullOutputPath}/{shaderName}'))
        shortShaderSrcPath = ConvertToRelativePath(shaderPathStr)
        shortShaderDstPath = ConvertToRelativePath(fullShaderDstPath)
        
        # Log the shader source name for clarity in the build process
        shaderSourceFileName = shaderPathStr[shaderPathStr.rfind("\\") + 1 : ]
        LogInfo(f'[ {shaderSourceFileName.upper()} ]')
        
        # Check dependencies. This function call returns a list of the dependencies which need to be
        # re-compiled. If it's empty, we know that the shader either has no dependencies or that no
        # dependencies need to be re-compiled
        dependenciesRequiringRecompilation = CheckShaderDependencies(shaderPath, checksums, dependencyComputedHashes)
        
        # Check if the shader was already compiled by referring to it's checksum (if it exists)
        if shortShaderSrcPath not in checksums:
            checksums[shortShaderSrcPath] = None
            
        existingHash = checksums[shortShaderSrcPath]
        
        shaderHandleAsBinary = open(shaderPath, "rb")
        newHash = None
        if shaderHandleAsBinary is not None:
            
            newHash = ShaderChecksum(shaderHandleAsBinary.read())
            if (existingHash == newHash) and (len(dependenciesRequiringRecompilation) == 0):
                LogInfo(f'Compilation skipped for shader with matching hash ({newHash}): "{os.path.normpath(shortShaderSrcPath)}"')
                continue
        
        # We only really care about the hash diffs if we had an existing one to begin with
        if existingHash != newHash:
            LogInfo(f'Found mismatched hash for shader source "{os.path.normpath(shortShaderSrcPath)}". Old hash {existingHash} vs. new hash {newHash}')
        elif len(dependenciesRequiringRecompilation) >= 0:
            dependenciesStr = ""
            for i, dependency in enumerate(dependenciesRequiringRecompilation):
                dependenciesStr += "\n" + (" " * 21) + f"{i + 1}) {dependency}"
            LogInfo(f'Found edited dependencies for shader source "{os.path.normpath(shortShaderSrcPath)}": {dependenciesStr}')
        else:
            LogError(f'Passed all checks to compile shader without finding a mismatched hash or updated dependency? Proceeding to compile shader "{shaderPath}"')
            
        newHashesFound = True
        
        # Checksum not found, we'll proceed to compile the shader
        os.makedirs(fullOutputPath, exist_ok=True)
        checksums[shortShaderSrcPath] = newHash
        
        CompileShaderByteCode(shaderPath, fullOutputPath, shaderName) # Shader source code compilation
        LogInfo(f'Compiled "{os.path.normpath(shortShaderSrcPath)}" into "{os.path.normpath(shortShaderDstPath)}.spv"')
        
        GenerateShaderMetadata(shaderPath, fullOutputPath, shaderName) # Shader metadata generation
        LogInfo(f'Generated metadata file for "{os.path.normpath(shortShaderSrcPath)}" into "{os.path.normpath(shortShaderDstPath)}"')
        
    # Add the newest dependency checksums
    for dependencyPath in dependencyComputedHashes:
        shortDependencyPath = ConvertToRelativePath(dependencyPath)
        checksums[shortDependencyPath] = dependencyComputedHashes[dependencyPath]
        
    # Only write to the checksum file if any checksums changed (or new shader files were added)
    if newHashesFound:
        json.dump(checksums, checksumFileHandle, indent = 2)
        LogInfo(f'Updated checksums file in "{os.path.normpath(checksumFilePath)}"')
        
    checksumFileHandle.close()
    
    LogBuildStep("SHADER COMPILATION/METADATA END")
    LogBuildStep("BUILDING SHADERS END")

if __name__ == "__main__":
    main()